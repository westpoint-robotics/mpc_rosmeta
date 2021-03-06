#include "continuation_gmres.hpp"


ContinuationGMRES::ContinuationGMRES(const NMPCModel model, const double horizon_max_length, const double alpha, const int horizon_division_num, const double difference_increment, const double zeta, const int dim_krylov) : MatrixFreeGMRES((model.dimControlInput()+model.dimConstraints())*horizon_division_num, dim_krylov)
{
    // Set dimensions and parameters.
    model_ = model;
    dim_state_ = model_.dimState();
    dim_control_input_ = model_.dimControlInput();
    dim_constraints_ = model_.dimConstraints();
    dim_control_input_and_constraints_ = dim_control_input_ + dim_constraints_;
    dim_solution_ = horizon_division_num * dim_control_input_and_constraints_;

    // Set parameters for horizon and the C/GMRES.
    horizon_max_length_ = horizon_max_length;
    alpha_ = alpha;
    horizon_division_num_ = horizon_division_num;
    difference_increment_ = difference_increment;
    zeta_ = zeta;
    dim_krylov_ = dim_krylov;

    // Allocate matrices and vectors.
    dx_vec_.resize(dim_state_);
    incremented_state_vec_.resize(dim_state_);
    state_mat_.resize(dim_state_, horizon_division_num_+1);
    lambda_mat_.resize(dim_state_, horizon_division_num_+1);
    solution_vec_.resize(dim_solution_);
    optimality_vec_.resize(dim_solution_);
    optimality_vec_1_.resize(dim_solution_);
    optimality_vec_2_.resize(dim_solution_);
    solution_update_vec_.resize(dim_solution_);

    // Initialize solution of the forward-difference GMRES.
    solution_update_vec_ = Eigen::VectorXd::Zero(dim_solution_);
}


void ContinuationGMRES::initSolution(const double initial_time, const Eigen::VectorXd& initial_state_vec, const Eigen::VectorXd& initial_guess_input_vec, const double convergence_radius, const int max_iteration)
{
    Eigen::VectorXd initial_solution_vec(dim_control_input_and_constraints_);
    InitCGMRES initializer(model_, difference_increment_, dim_krylov_);

    initial_time_ = initial_time;
    initializer.solve0stepNOCP(initial_time, initial_state_vec, initial_guess_input_vec, convergence_radius, max_iteration, initial_solution_vec);
    for(int i=0; i<horizon_division_num_; i++){
        // Intialize the solution_vec_.
        solution_vec_.segment(i*dim_control_input_and_constraints_, dim_control_input_and_constraints_) = initial_solution_vec;    
        // Intialize the optimality_vec_.
        optimality_vec_.segment(i*dim_control_input_and_constraints_, dim_control_input_and_constraints_) = initializer.getOptimalityErrorVec(initial_time, initial_state_vec, initial_solution_vec);
    }
}


void ContinuationGMRES::controlUpdate(const double current_time, const double sampling_period, const Eigen::VectorXd& current_state_vec, Eigen::Ref<Eigen::VectorXd> optimal_control_input_vec)
{
    // Predict the incremented state.
    incremented_time_ = current_time + difference_increment_;
    model_.stateFunc(current_time, current_state_vec, solution_vec_.segment(0, dim_control_input_), dx_vec_);
    incremented_state_vec_ = current_state_vec + difference_increment_*dx_vec_;

    // Solves the matrix-free GMRES and updates the solution.
    forwardDifferenceGMRES(current_time, current_state_vec, solution_vec_, solution_update_vec_);
    solution_vec_ += sampling_period * solution_update_vec_;
    optimal_control_input_vec = solution_vec_.segment(0, dim_control_input_);
}


double ContinuationGMRES::getError(const double current_time, const Eigen::VectorXd& current_state_vec)
{
    Eigen::VectorXd error_vec(dim_solution_);

    computeOptimalityError(current_time, current_state_vec, solution_vec_, error_vec);

    return error_vec.norm();
}


void ContinuationGMRES::computeOptimalityError(const double time_param, const Eigen::VectorXd& state_vec, const Eigen::VectorXd& current_solution_vec, Eigen::Ref<Eigen::VectorXd> optimality_vec)
{
    // Set and discretize the horizon.
    double horizon_length = horizon_max_length_ * (1.0 - std::exp(- alpha_ * (time_param - initial_time_)));
    double delta_tau = horizon_length / horizon_division_num_;

    // Sompute the state trajectory over the horizon on the basis of the control_input_vec and the current_state_vec.
    state_mat_.col(0) = state_vec;
    double tau=time_param;
    for(int i=0; i<horizon_division_num_; i++, tau+=delta_tau){
        model_.stateFunc(tau, state_mat_.col(i), current_solution_vec.segment(i*dim_control_input_and_constraints_, dim_control_input_), dx_vec_);
        state_mat_.col(i+1) = state_mat_.col(i) + delta_tau * dx_vec_;
    }

    // Compute the Lagrange multiplier and the optimality condition over the horizon on the basis of the control_input_vec and the current_state_vec.
    model_.phixFunc(tau, state_mat_.col(horizon_division_num_), lambda_mat_.col(horizon_division_num_));
    for(int i=horizon_division_num_-1; i>=0; i--, tau-=delta_tau){
        model_.hxFunc(tau, state_mat_.col(i), current_solution_vec.segment(i*dim_control_input_and_constraints_, dim_control_input_and_constraints_), lambda_mat_.col(i+1), dx_vec_);
        lambda_mat_.col(i) = lambda_mat_.col(i+1) + delta_tau * dx_vec_;
        model_.huFunc(tau, state_mat_.col(i), current_solution_vec.segment(i*dim_control_input_and_constraints_, dim_control_input_and_constraints_), lambda_mat_.col(i+1), optimality_vec.segment(i*dim_control_input_and_constraints_, dim_control_input_and_constraints_));
    }
}


void ContinuationGMRES::bFunc(const double time_param, const Eigen::VectorXd& state_vec, const Eigen::VectorXd& current_solution_vec, Eigen::Ref<Eigen::VectorXd> equation_error_vec)
{
    computeOptimalityError(time_param, state_vec, current_solution_vec, optimality_vec_);
    computeOptimalityError(incremented_time_, incremented_state_vec_, current_solution_vec, optimality_vec_1_);
    computeOptimalityError(incremented_time_, incremented_state_vec_, current_solution_vec+difference_increment_*solution_update_vec_, optimality_vec_2_);

    equation_error_vec = (1/difference_increment_-zeta_) *optimality_vec_ - optimality_vec_2_/difference_increment_;
}


inline void ContinuationGMRES::axFunc(const double time_param, const Eigen::VectorXd& state_vec, const Eigen::VectorXd& current_solution_vec, const Eigen::VectorXd& direction_vec, Eigen::Ref<Eigen::VectorXd> forward_difference_error_vec)
{
    computeOptimalityError(incremented_time_, incremented_state_vec_, current_solution_vec+difference_increment_*direction_vec, optimality_vec_2_);
    forward_difference_error_vec = (optimality_vec_2_ - optimality_vec_1_) / difference_increment_;
}