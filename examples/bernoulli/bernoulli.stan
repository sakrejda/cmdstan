functions {
  real f(real x, real y) {
    real d;
    d = x*y;
    return d;
  }
  int g(int x, int y) {
    int d;
    d = x*y;
    return d;
  }
}

data { 
  int<lower=0> N; 
  int<lower=0,upper=1> y[N];
} 
parameters {
  real<lower=0,upper=1> theta;
} 
model {
  theta ~ beta(1,1);
  for (n in 1:N) 
    y[n] ~ bernoulli(theta);
}
