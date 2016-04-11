<a href="http://mc-stan.org"><img src="https://github.com/stan-dev/stan/blob/master/logos/stanlogo-main.png?raw=true" alt="Stan Logo" /></a>

# Postgres CmdStan *fork*

<b>CmdStan</b> is the command line interface to Stan, a C++ package providing

* full Bayesian inference using the No-U-Turn sampler (NUTS), a variant of Hamiltonian Monte Carlo (HMC),
* approximate Bayesian inference using automatic differentiation variational inference (ADVI),
* penalized maximum likelihood estimation (MLE) using L-BFGS optimization,
* a full first- and higher-order automatic differentiation library based on C++ template overloads, and
* a supporting fully-templated matrix, linear algebra, and probability special function library.

This *FORK* is modified to write (almost) all output to a postgres
database using the libpq/libpqxx libraries.  At the moment performance
is barely adequate for some uses and highly dependent on how many
parameter are in play and the configuration of the postgres backend
(even when writing locally with no network delays).  Roughly I've seen
performance between 500 and 2500 rows per second on the parameter output
write with a row being a single parameter at a single iteration.  The
target is to make write speeds adequate for medium/large models (up to
hundreds of thousands of parameters) which are challenging for Stan
so run times around 8 hours.  For very simple models Stan can bury
the backend and while there is no data loss (assuming there is enough
RAM for the work queue) it might take a long time to write the output
to the database.  

### Home Page
Stan's home page, with links to everything you'll need to use Stan is:

[http://mc-stan.org/](http://mc-stan.org/)

### Interfaces
There are separate repositories here on GitHub for interfaces:
* RStan (R interface)
* PyStan (Python interface)
* CmdStan (command-line/shell interface)

### Source Repository
CmdStan's source-code repository is hosted here on GitHub.

### Licensing
The core Stan C++ code and CmdStan are licensed under new BSD.


## Installation using git

Sorry, there's no easy way at this point.  If you haven't tried an
example CmdStan model yet (all the way to compilation/running it) check
that out first.  Also:

- Have a postgres database you know how to connectp to (e.g.-using psql
  and a URI string). 
- Have libpqxx installed correctly, the default makefile assumes
  /usr/local
- git clone https://github.com/sakrejda/cmdstan.git
- git submodule update --init --recursive

## Try it out

This is only really worth it if you run models that produce output too
large to load into RAM, have a postgres database around, and are having
rstan fail due to RAM/memory issues.

make examples/bernoulli/bernoulli
examples/bernoulli/bernoulli sample data file=examples/bernoulli/bernoulli.data.R \
  output file="<URI>"



