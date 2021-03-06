#ifndef STAN_INTERFACE_CALLBACKS_WRITER_PSQL_WRITER_HPP
#define STAN_INTERFACE_CALLBACKS_WRITER_PSQL_WRITER_HPP

#include <stan/interface_callbacks/writer/base_writer.hpp>
#include <stan/interface_callbacks/writer/psql_writer_helpers.hpp>
#include <pqxx/pqxx>
#include <ostream>
#include <chrono>
#include <vector>
#include <queue>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>


namespace stan {
  namespace interface_callbacks {
    namespace writer {

      /**
       * psql_writer writes to the postgres database specific by the URI
       * string.  Since it relies on the standard postgres library, the
       * URI lets you rely on any of the usual authentication methods
       * for psql.  For local connections this doesn't matter but the
       * goal is to make remote writes doable and reliable so SSL/TLS
       * client/server authentication and encryption are in the scope of
       * the writer.
       *
       * The database side strategy is to identify each run with a
       * (short) hash that gets written with each key/value/index/type
       * message.  
       *
       * The tables we use: 
       *  - runs
       *  - key_value (as hash, key, idx, row, column, double, string, int)
       *  - parameter_names
       *  - parameter_samples
       *  - messages
       *
       */

      class psql_writer : public base_writer {
      public:
        /**
         * Constructor.
         *
         * @param uri std::string passed to the psql library to establish a
         *   connection. 
         * @param id std::string passed as a user-generated tag for the
         *   runs table.  Not used as an index internally. 
         */
        psql_writer(const std::string& uri = "", const std::string id = ""):
            uri__(uri), id__(id), iteration__(0), n_threads__(1), 
            finished__(false) {

          conn__ = new pqxx::connection(uri);
          conn__->perform(do_sql(create_runs_sql));
          conn__->perform(do_sql(create_key_value_sql));
          conn__->perform(do_sql(create_parameter_names_sql));
          conn__->perform(do_sql(create_parameter_samples_sql));
          conn__->perform(do_sql(create_messages_sql));
          
          pqxx::work write(*conn__, "run_write");
          pqxx::result runs_result = write.exec("INSERT INTO runs (id) VALUES ('" + id__ + "') RETURNING hash;");
          hash__ = runs_result[0][0].c_str();   
          write.commit();

          conn__->prepare("write_key_value", write_key_value_sql);
          conn__->prepare("write_parameter_name", write_parameter_name_sql);
          conn__->prepare("write_parameter_sample", write_parameter_sample_sql);
          conn__->prepare("write_message", write_message_sql);
          
        }

        /**
         * Copy constructor.  Not a regular deep copy.  We keep the same
         * hash, unless it is modified.  We create a new main connection, we create new
         * thread which launch their own connections.  If multiple
         * copies are used to write iterations the iteration number will
         * be inconsistent (each instance counts separately!).
         **/
        psql_writer(const psql_writer& other, std::string hash = "") :
            uri__(other.uri__), id__(other.id__), iteration__(0), 
            hash__(other.hash__), finished__(false) {
         
          start__ = std::chrono::system_clock::now();
 
          if (hash != "") 
            hash__ = hash;

          conn__ = new pqxx::connection(uri__);
          conn__->perform(do_sql(create_runs_sql));
          conn__->perform(do_sql(create_key_value_sql));
          conn__->perform(do_sql(create_parameter_names_sql));
          conn__->perform(do_sql(create_parameter_samples_sql));
          conn__->perform(do_sql(create_messages_sql));

          conn__->prepare("write_key_value", write_key_value_sql);
          conn__->prepare("write_parameter_name", write_parameter_name_sql);
          conn__->prepare("write_parameter_sample", write_parameter_sample_sql);
          conn__->prepare("write_message", write_message_sql);
          
        }

        /**
         * Assignment is disallowed.
         **/
        psql_writer& operator=(const psql_writer& other) = delete;

        /**
         * Move constructor is disallowed
         **/
        psql_writer& operator=(psql_writer&& other) = delete;

        ~psql_writer() {
          while(1) { 
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            mutex_samples__.lock();
            if (samples__.size() == 0) {
              mutex_samples__.unlock();
              finished__ = true;
              break;
            } else {
              mutex_samples__.unlock();
            }
          }
          for (unsigned int i=0; i < write_threads__.size(); ++i) {
            write_threads__[i].join();
          }

          end__ = std::chrono::system_clock::now();
          auto e = std::chrono::duration_cast<std::chrono::milliseconds>(end__ - start__);
          (*this)("elapsed time", static_cast<int>(e.count()));

          conn__->disconnect();
          delete conn__;

        }

        void operator()(const std::string& key, double value) {
          conn__->perform(write_key_double(hash__, key, value));
        }

        void operator()(const std::string& key, int value) {
          conn__->perform(write_key_integer(hash__, key, value));
          
        }

        void operator()(const std::string& key, const std::string& value) {
          conn__->perform(write_key_string(hash__, key, value));
        }

        void operator()(const std::string& key,
                        const double* values,
                        int n_values
        ) {
          conn__->perform(write_key_doubles_n(hash__, key, values, n_values));
        }

        void operator()(const std::string& key, const double* values,
                        int n_rows, int n_cols) {
          conn__->perform(write_key_doubles_rows_columns(hash__, key, values, n_rows, n_cols));
        }

        void operator()(const std::vector<std::string>& names) {
          names__ = names;

          if (names__.size() >= prepared_statement_batch_size) {
            big_prepared_sql__ = write_parameter_sample_sql_stub;
            long offset;
            for (unsigned long i = 0; i < prepared_statement_batch_size; ++i) {
              offset = i*4;
              big_prepared_sql__ += "($" + std::to_string(offset+1) + "," +
                                    " $" + std::to_string(offset+2) + "," +
                                    " $" + std::to_string(offset+3) + "," +
                                    " $" + std::to_string(offset+4) + ") ";
              if (i < prepared_statement_batch_size - 1)
                big_prepared_sql__ += ", ";
              else
                big_prepared_sql__ += ";";
            }
            conn__->prepare("write_parameter_iteration_batch", big_prepared_sql__);
          }

          long p = names__.size() % prepared_statement_batch_size;
          if (p != 0) {
            small_prepared_sql__ = write_parameter_sample_sql_stub;
            long offset;
            for (unsigned long i = 0; i < p; ++i) {
              offset = i*4;
              small_prepared_sql__ += "($" + std::to_string(offset+1) + "," +
                                      " $" + std::to_string(offset+2) + "," +
                                      " $" + std::to_string(offset+3) + "," +
                                      " $" + std::to_string(offset+4) + ") ";
              if (i < p - 1)
                small_prepared_sql__ += ", ";
              else
                small_prepared_sql__ += ";";
            }
            conn__->prepare("write_parameter_iteration_final", small_prepared_sql__);
          }

          for (unsigned int i=0; i < n_threads__; ++i) {
            write_threads__.emplace_back(std::thread(&psql_writer::consume_samples, this));
          }

          conn__->perform(write_parameter_names(hash__, names));
        }

        void operator()(const std::vector<double>& state) {
          mutex_samples__.lock();
          samples__.push(state);
          mutex_samples__.unlock();
        }

        void operator()() { }

        void operator()(const std::string& message) {
          conn__->perform(write_message(hash__, message));
        }

      private:
        std::chrono::time_point<std::chrono::system_clock> start__, end__;
        pqxx::connection* conn__;
        std::vector<std::string> names__;
        std::string uri__;
        std::string hash__;
        std::string id__;
        std::string big_prepared_sql__;
        std::string small_prepared_sql__;
        int iteration__;
        int n_threads__;

        std::vector<std::thread> write_threads__;
        std::mutex mutex_samples__;
        std::atomic<bool> finished__;
        std::queue<std::vector<double> > samples__;

        void consume_samples() {
          int iteration;
          std::vector<double> state;
          pqxx::connection* conn = new pqxx::connection(uri__);
          conn->prepare("write_parameter_iteration_batch", big_prepared_sql__);
          conn->prepare("write_parameter_iteration_final", small_prepared_sql__);
          while(!finished__) {
            mutex_samples__.lock();
            if (samples__.size() > 0) {
              state = samples__.front(); 
              samples__.pop();
              ++iteration__;
              iteration = iteration__;
              mutex_samples__.unlock(); 
              conn->perform(write_parameter_samples(hash__, iteration, names__, 
                    state, prepared_statement_batch_size));
            } else {
              mutex_samples__.unlock();
            }
          } 
          delete conn;
        }

        static const std::string create_runs_sql;
        static const std::string create_key_value_sql;
        static const std::string create_parameter_names_sql;
        static const std::string create_parameter_samples_sql;
        static const std::string create_messages_sql;

        static const std::string write_key_value_sql;
        static const std::string write_parameter_name_sql;
        static const std::string write_parameter_sample_sql;
        static const std::string write_parameter_sample_sql_stub;
        static const std::string write_message_sql;

        static const long prepared_statement_batch_size;
      };

      const long psql_writer::prepared_statement_batch_size = 10000;

      const std::string psql_writer::create_runs_sql = "CREATE TABLE IF NOT EXISTS "
        "runs("
        "hash SERIAL PRIMARY KEY,"
        "timestamp TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "id TEXT NOT NULL"
      ");";
      const std::string psql_writer::create_key_value_sql = "CREATE TABLE IF NOT EXISTS "
        "key_value("
        "hash INT REFERENCES runs,"
        "key VARCHAR(300),"
        "idx INTEGER,"
        "row_idx INTEGER,"
        "col_idx INTEGER,"
        "double DOUBLE PRECISION,"
        "string VARCHAR(300),"
        "integer INTEGER"
      ");";
      const std::string psql_writer::create_parameter_names_sql = "CREATE TABLE IF NOT EXISTS "
        "parameter_names("
        "hash INT REFERENCES runs,"
        "name VARCHAR(300)"
      ");";
      const std::string psql_writer::create_parameter_samples_sql = "CREATE TABLE IF NOT EXISTS "
        "parameter_samples("
        "hash INTEGER, "
        "iteration INTEGER, "
        "name VARCHAR(301), "
        "value DOUBLE PRECISION"
      ");";
      const std::string psql_writer::create_messages_sql = "CREATE TABLE IF NOT EXISTS "
        "messages("
        "hash INTEGER REFERENCES runs,"
        "message TEXT"
      ");";

      const std::string psql_writer::write_key_value_sql = "INSERT INTO key_value "
        "(hash, key, idx, row_idx, col_idx, double, string, integer)"
        " VALUES "
        "($1, $2, $3, $4, $5, $6, $7, $8);";
      const std::string psql_writer::write_parameter_name_sql = "INSERT INTO parameter_names "
        "(hash, name)"
        " VALUES "
        "($1, $2);";
      const std::string psql_writer::write_parameter_sample_sql = "INSERT INTO parameter_samples "
        "(hash, iteration, name, value)"
        " VALUES "
        "($1, $2, $3, $4);";
      const std::string psql_writer::write_parameter_sample_sql_stub = "INSERT "
        "INTO parameter_samples "
        "(hash, iteration, name, value)"
        " VALUES ";
      const std::string psql_writer::write_message_sql = "INSERT INTO messages "
        "(hash, message)"
        " VALUES "
        "($1, $2);";

    }
  }
}

#endif
