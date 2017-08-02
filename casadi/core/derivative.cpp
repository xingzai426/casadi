/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "derivative.hpp"

using namespace std;

namespace casadi {

  Function CentralDiff::create(const std::string& name, int n, const Dict& opts) {
    return Function::create(new CentralDiff(name, n), opts);
  }

  CentralDiff::CentralDiff(const std::string& name, int n)
    : FunctionInternal(name), n_(n) {
  }

  CentralDiff::~CentralDiff() {
  }

  Options CentralDiff::options_
  = {{&FunctionInternal::options_},
     {{"stepsize",
       {OT_DOUBLE,
        "Perturbation size [default: 1e-8]"}},
      {"second_order_stepsize",
       {OT_DOUBLE,
        "Second order perturbation size [default: 1e-3]"}},
      {"scheme",
       {OT_STRING,
        "Differencing scheme [default: 'central']"}}
     }
  };

  void CentralDiff::init(const Dict& opts) {
    // Call the initialization method of the base class
    FunctionInternal::init(opts);

    // Default options
    h_ = 1e-8;
    h2_ = 1e-3;

    // Read options
    for (auto&& op : opts) {
      if (op.first=="stepsize") {
        h_ = op.second;
      } else if (op.first=="second_order_stepsize") {
        h2_ = op.second;
      } else if (op.first=="scheme") {
        casadi_warning("Option 'scheme' currently ignored");
      }
    }

    // Allocate work vector for (perturbed) inputs and outputs
    alloc_w((n_calls()+1) * f().nnz_in(), true);
    alloc_w((n_calls()+1) * f().nnz_out(), true);

    // Work vectors for seeds/sensitivities
    alloc_arg(derivative_of_.n_in(), true);
    alloc_res(derivative_of_.n_out(), true);

    // Allocate sufficient temporary memory for function evaluation
    alloc(f());
  }

  Sparsity CentralDiff::get_sparsity_in(int i) {
    int n_in = derivative_of_.n_in(), n_out = derivative_of_.n_out();
    if (i<n_in) {
      // Non-differentiated input
      return derivative_of_.sparsity_in(i);
    } else if (i<n_in+n_out) {
      // Non-differentiated output
      return derivative_of_.sparsity_out(i-n_in);
    } else {
      // Seeds
      return repmat(derivative_of_.sparsity_in(i-n_in-n_out), 1, n_);
    }
  }

  Sparsity CentralDiff::get_sparsity_out(int i) {
    return repmat(derivative_of_.sparsity_out(i), 1, n_);
  }

  double CentralDiff::default_in(int ind) const {
    if (ind<derivative_of_.n_in()) {
      return derivative_of_.default_in(ind);
    } else {
      return 0;
    }
  }

  size_t CentralDiff::get_n_in() {
    return derivative_of_.n_in() + derivative_of_.n_out() + derivative_of_.n_in();
  }

  size_t CentralDiff::get_n_out() {
    return derivative_of_.n_out();
  }

  std::string CentralDiff::get_name_in(int i) {
    int n_in = derivative_of_.n_in(), n_out = derivative_of_.n_out();
    if (i<n_in) {
      return derivative_of_.name_in(i);
    } else if (i<n_in+n_out) {
      return "out_" + derivative_of_.name_out(i-n_in);
    } else {
      return "fwd_" + derivative_of_.name_in(i-n_in-n_out);
    }
  }

  std::string CentralDiff::get_name_out(int i) {
    return "fwd_" + derivative_of_.name_out(i);
  }

  Function CentralDiff::get_forward(int nfwd, const std::string& name,
                                   const std::vector<std::string>& inames,
                                   const std::vector<std::string>& onames,
                                   const Dict& opts) const {
    Dict opts_mod = opts;
    opts_mod["stepsize"] = h2_;
    return Function::create(new CentralDiff(name, nfwd), opts_mod);
  }

  void CentralDiff::eval(void* mem, const double** arg, double** res, int* iw, double* w) const {
    // Shorthands
    int n_in = derivative_of_.n_in(), n_out = derivative_of_.n_out(), n_calls = this->n_calls();

    // Non-differentiated input
    const double* f_arg = w;
    for (int j=0; j<n_in; ++j) {
      const int nnz = derivative_of_.nnz_in(j);
      casadi_copy(*arg++, nnz, w);
      w += nnz;
    }

    // Non-differentiated output
    const double* f_res = w;
    for (int j=0; j<n_out; ++j) {
      const int nnz = derivative_of_.nnz_out(j);
      casadi_copy(*arg++, nnz, w);
      w += nnz;
    }

    // Forward seeds
    const double** seed = arg; arg += n_in;

    // Forward sensitivities
    double** sens = res; res += n_out;

    // Copy sensitivitity arguments to temporary vectors to allow modification
    copy_n(seed, n_in, arg);
    seed = arg; arg += n_in;
    copy_n(sens, n_out, res);
    sens = res; res += n_out;

    // Work vectors for perturbed inputs and outputs
    double* f_arg_pert = w; w += n_calls * f().nnz_in();
    double* f_res_pert = w; w += n_calls * f().nnz_out();

    // For each derivative direction
    for (int i=0; i<n_; ++i) {
      // Perturb function argument (depends on differentiation algorithm)
      perturb(f_arg, f_arg_pert, seed);

      // Function evaluation
      double* f_arg_pert1 = f_arg_pert;
      double* f_res_pert1 = f_res_pert;
      for (int c=0; c<n_calls; ++c) {
        // Function inputs
        for (int j=0; j<n_in; ++j) {
          arg[j] = f_arg_pert1;
          f_arg_pert1 += f().nnz_in(j);
        }
        // Function outputs
        for (int j=0; j<n_out; ++j) {
          res[j] = f_res_pert1;
          f_res_pert1 += f().nnz_out(j);
        }
        // Call function
        f()(arg, res, iw, w, 0);
      }

      // Calculate finite difference approximation
      finalize(f_res, f_res_pert, sens);

      // Proceed to the next direction
      for (int j=0; j<n_in; ++j) if (seed[j]) seed[j] += derivative_of_.nnz_in(j);
      for (int j=0; j<n_out; ++j) if (sens[j]) sens[j] += derivative_of_.nnz_out(j);
    }
  }

  void CentralDiff::perturb(const double* f_arg, double* f_arg_pert, const double** seed) const {
    int n_in = derivative_of_.n_in();
    for (int sign=0; sign<2; ++sign) {
      const double* f_arg1 = f_arg;
      for (int j=0; j<n_in; ++j) {
        const int nnz = derivative_of_.nnz_in(j);
        casadi_copy(f_arg1, nnz, f_arg_pert);
        casadi_axpy(nnz, sign ? -h_/2 : h_/2, seed[j], f_arg_pert);
        f_arg1 += nnz;
        f_arg_pert += nnz;
      }
    }
  }

  void CentralDiff::finalize(const double* f_res, const double* f_res_pert, double** sens) const {
    const double* f_res_pert1 = f_res_pert + derivative_of_.nnz_out();
    int n_out = derivative_of_.n_out();
    for (int j=0; j<n_out; ++j) {
      const int nnz = derivative_of_.nnz_out(j);
      casadi_copy(f_res_pert, nnz, sens[j]);
      f_res_pert += nnz;
      casadi_axpy(nnz, -1., f_res_pert1, sens[j]);
      casadi_scal(nnz, 1/h_, sens[j]);
      f_res_pert1 += nnz;
    }
  }

} // namespace casadi
