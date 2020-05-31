#include <iostream>
#include <string>
#include <cmath>
// For External Library
#include <torch/torch.h>
// For Original Header
#include "loss.hpp"

// Define Namespace
namespace F = torch::nn::functional;


// -----------------------------------
// class{Loss} -> constructor
// -----------------------------------
Loss::Loss(const std::string loss){
    if (loss == "l1"){
        this->flag = 0;
    }
    else if (loss == "l2"){
        this->flag = 1;
    }
    else if (loss == "ssim"){
        this->flag = 2;
    }
    else{
        std::cerr << "Error : The loss fuction isn't defined right." << std::endl;
        std::exit(1);
    }
}


// -----------------------------------
// class{Loss} -> operator
// -----------------------------------
torch::Tensor Loss::operator()(torch::Tensor &input, torch::Tensor &target){
    if (this->flag == 0){
        static auto criterion = torch::nn::L1Loss(torch::nn::L1LossOptions().reduction(torch::kMean));
        return criterion(input, target);
    }
    else if (this->flag == 1){
        static auto criterion = torch::nn::MSELoss(torch::nn::MSELossOptions().reduction(torch::kMean));
        return criterion(input, target);
    }
    static auto criterion = SSIMLoss(input.size(1), input.device());
    return criterion(input, target);
}


// --------------------------------
// class{SSIMLoss} -> constructor
// --------------------------------
SSIMLoss::SSIMLoss(const size_t nc_, const torch::Device device, const size_t window_size_, const float gauss_std_, const float c1_base_, const float c2_base_){

    this->nc = nc_;
    this->window_size = window_size_;
    this->gauss_std = gauss_std_;
    this->c1_base = c1_base_;
    this->c2_base = c2_base_;

    float *gauss_list = new float[this->window_size];
    auto Gaussian_PDF = [](float mean, float std, float x){ return std::exp(- (x - mean) * (x - mean) / (2.0 * std * std)); };
    for (size_t i = 0; i < this->window_size; i++){
        gauss_list[i] = Gaussian_PDF(this->window_size/2, this->gauss_std, (float)i);
    }

    torch::Tensor tensor;
    tensor = torch::from_blob(gauss_list, {(long int)this->window_size}, torch::kFloat);
    tensor = tensor / tensor.sum();
    tensor = tensor.unsqueeze(/*dim=*/1);
    tensor = tensor.mm(tensor.t());
    tensor = tensor.unsqueeze(/*dim=*/0).unsqueeze(/*dim=*/0);
    tensor = tensor.expand({(long int)this->nc, 1, (long int)this->window_size, (long int)this->window_size}).contiguous();

    // -------------------------------------------------------------------------------------
    // (Default) if window size is 11 and gauss_std is 1.5, ...
    // 
    //          | 1.0576 7.8144 37.022 112.46 219.05 273.56 219.05 112.46 37.022 7.8144 1.0576 |
    //          | 7.8144 57.741 273.56 831.01 1618.6 2021.4 1618.6 831.01 273.56 57.741 7.8144 |
    //          | 37.022 273.56 1296.1 3937.1 7668.4 9576.6 7668.4 3937.1 1296.1 273.56 37.022 |
    //          | 112.46 831.01 3937.1 11960. 23294. 29091. 23294. 11960. 3937.1 831.01 112.46 |
    //          | 219.05 1618.6 7668.4 23294. 45371. 56662. 45371. 23294. 7668.4 1618.6 219.05 |
    // window = | 273.56 2021.4 9576.6 29091. 56662. 70762. 56662. 29091. 9576.6 2021.4 273.56 | * 0.000001
    //          | 219.05 1618.6 7668.4 23294. 45371. 56662. 45371. 23294. 7668.4 1618.6 219.05 |
    //          | 112.46 831.01 3937.1 11960. 23294. 29091. 23294. 11960. 3937.1 831.01 112.46 |
    //          | 37.022 273.56 1296.1 3937.1 7668.4 9576.6 7668.4 3937.1 1296.1 273.56 37.022 |
    //          | 7.8144 57.741 273.56 831.01 1618.6 2021.4 1618.6 831.01 273.56 57.741 7.8144 |
    //          | 1.0576 7.8144 37.022 112.46 219.05 273.56 219.05 112.46 37.022 7.8144 1.0576 |
    // 
    // -------------------------------------------------------------------------------------

    this->window = tensor.to(device).detach();

    delete[] gauss_list;

}


// ----------------------------------------------------
// class{SSIMLoss} -> function{Structural_Similarity}
// ----------------------------------------------------
torch::Tensor SSIMLoss::Structural_Similarity(torch::Tensor &image1, torch::Tensor &image2){

    // (1) Calculation of Mean
    torch::Tensor mu1 = F::conv2d(image1, this->window, F::Conv2dFuncOptions().padding(this->window_size/2).groups(this->nc));
    torch::Tensor mu2 = F::conv2d(image2, this->window, F::Conv2dFuncOptions().padding(this->window_size/2).groups(this->nc));
    torch::Tensor mu1_sq = mu1.pow(2.0);
    torch::Tensor mu2_sq = mu2.pow(2.0);
    torch::Tensor mu1_mu2 = mu1 * mu2;

    // (2) Calculation of Variance and Covariance
    torch::Tensor var1 = F::conv2d(image1*image1, this->window, F::Conv2dFuncOptions().padding(this->window_size/2).groups(this->nc)) - mu1_sq;
    torch::Tensor var2 = F::conv2d(image2*image2, this->window, F::Conv2dFuncOptions().padding(this->window_size/2).groups(this->nc)) - mu2_sq;
    torch::Tensor covar = F::conv2d(image1*image2, this->window, F::Conv2dFuncOptions().padding(this->window_size/2).groups(this->nc)) - mu1_mu2;

    // (3) Calculation of SSIM
    float c1 = this->c1_base * this->c1_base;
    float c2 = this->c2_base * this->c2_base;
    torch::Tensor ssim = (2.0 * mu1_mu2 + c1) * (2.0 * covar + c2) / ((mu1_sq + mu2_sq + c1) * (var1 + var2 + c2));

    return ssim.mean();

}


// ----------------------------
// class{SSIMLoss} -> operator
// ----------------------------
torch::Tensor SSIMLoss::operator()(torch::Tensor &input, torch::Tensor &target){
    return -this->Structural_Similarity(input, target);  // -1.0<=SSIM<=1.0 (-1.0 is best matching)
}