#include <iostream>                    // std::cout
#include <fstream>                     // std::ifstream, std::ofstream
#include <filesystem>                  // std::filesystem
#include <string>                      // std::string
#include <tuple>                       // std::tuple
#include <chrono>                      // std::chrono
#include <utility>                     // std::pair
// For External Library
#include <torch/torch.h>               // torch
#include <boost/program_options.hpp>   // boost::program_options
// For Original Header
#include "networks.hpp"                // GAN_Generator, GAN_Discriminator
#include "transforms.hpp"              // transforms_Compose
#include "datasets.hpp"                // datasets::ImageFolderWithPaths
#include "dataloader.hpp"              // DataLoader::ImageFolderWithPaths
#include "visualizer.hpp"              // visualizer
#include "progress.hpp"                // progress

// Define Namespace
namespace fs = std::filesystem;
namespace po = boost::program_options;

// Function Prototype
std::tuple<torch::Tensor, torch::Tensor, torch::Tensor> AnomalyScore(torch::Tensor image, torch::Tensor fake_image, GAN_Discriminator &dis, const float Lambda=0.1);


// ---------------
// Test Function
// ---------------
void test(po::variables_map &vm, torch::Device &device, GAN_Generator &gen, GAN_Discriminator &dis, std::vector<transforms_Compose> &transform){

    constexpr std::pair<float, float> output_range = {-1.0, 1.0};  // range of the value in output images

    // (0) Initialization and Declaration
    size_t search_epoch;
    float ave_anomaly_score, ave_res_loss, ave_dis_loss;
    double seconds, ave_time;
    std::string path, result_dir, fname;
    std::string dataroot;
    std::ofstream ofs, ofs_score;
    std::chrono::system_clock::time_point start, end;
    std::tuple<torch::Tensor, std::vector<std::string>> data;
    std::tuple<torch::Tensor, torch::Tensor, torch::Tensor> anomaly_score_with_alpha;
    torch::Tensor image, z, output;
    torch::Tensor loss, anomaly_score, res_loss, dis_loss;
    datasets::ImageFolderWithPaths dataset;
    DataLoader::ImageFolderWithPaths dataloader;
    progress::display *show_progress;

    // (1) Get Test Dataset
    dataroot = "datasets/" + vm["dataset"].as<std::string>() + '/' + vm["test_dir"].as<std::string>();
    dataset = datasets::ImageFolderWithPaths(dataroot, transform);
    dataloader = DataLoader::ImageFolderWithPaths(dataset, /*batch_size_=*/1, /*shuffle_=*/false, /*num_workers_=*/0);
    std::cout << "total test images : " << dataset.size() << std::endl << std::endl;

    // (2) Get Model
    path = "checkpoints/" + vm["dataset"].as<std::string>() + "/models/epoch_" + vm["test_load_epoch"].as<std::string>() + "_gen.pth"; torch::load(gen, path, device);
    path = "checkpoints/" + vm["dataset"].as<std::string>() + "/models/epoch_" + vm["test_load_epoch"].as<std::string>() + "_dis.pth"; torch::load(dis, path, device);

    // (3) Initialization of Value
    ave_anomaly_score = 0.0;
    ave_res_loss = 0.0;
    ave_dis_loss = 0.0;
    ave_time = 0.0;
    search_epoch = vm["test_search_epoch"].as<size_t>();

    // (4) Initialization of Latent Variables
    z = torch::randn({1, (long int)vm["nz"].as<size_t>()}).to(device);
    z.requires_grad_(true);
    auto z_optimizer = torch::optim::Adam({z}, torch::optim::AdamOptions(vm["lr_z"].as<float>()).betas({vm["beta1"].as<float>(), vm["beta2"].as<float>()}));

    // (5) Tensor Forward
    gen->eval();
    dis->eval();
    result_dir = vm["test_result_dir"].as<std::string>();  fs::create_directories(result_dir);
    ofs.open(result_dir + "/loss.txt", std::ios::out);
    ofs_score.open(result_dir + "/anomaly_score.txt", std::ios::out);
    while (dataloader(data)){
        
        image = std::get<0>(data).to(device);
        
        show_progress = new progress::display(/*count_max_=*/search_epoch, /*header1=*/std::get<1>(data).at(0), /*header2=*/"|", /*loss_=*/{"loss", "res", "dis"});
        torch::cuda::synchronize();
        start = std::chrono::system_clock::now();
        
        for (size_t i = 0; i < search_epoch; i++){
            output = gen->forward(z);
            anomaly_score_with_alpha = AnomalyScore(image, output, dis, vm["test_Lambda"].as<float>());
            loss = std::get<0>(anomaly_score_with_alpha);
            res_loss = std::get<1>(anomaly_score_with_alpha);
            dis_loss = std::get<2>(anomaly_score_with_alpha);
            z_optimizer.zero_grad();
            loss.backward();
            z_optimizer.step();
            show_progress->increment(/*loss_value=*/{loss.item<float>(), res_loss.item<float>(), dis_loss.item<float>()});
        }
        output = gen->forward(z);
        anomaly_score_with_alpha = AnomalyScore(image, output, dis, vm["test_Lambda"].as<float>());
        anomaly_score = std::get<0>(anomaly_score_with_alpha);
        res_loss = std::get<1>(anomaly_score_with_alpha);
        dis_loss = std::get<2>(anomaly_score_with_alpha);

        torch::cuda::synchronize();
        end = std::chrono::system_clock::now();
        seconds = (double)std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() * 0.001 * 0.001;
        delete show_progress;
        
        ave_anomaly_score += anomaly_score.item<float>();
        ave_res_loss += res_loss.item<float>();
        ave_dis_loss += dis_loss.item<float>();
        ave_time += seconds;

        ofs << '<' << std::get<1>(data).at(0) << "> anomaly_score:" << anomaly_score.item<float>() << " res:" << res_loss.item<float>() << " dis:" << dis_loss.item<float>() << " (time:" << ave_time << ')' << std::endl;
        ofs_score << anomaly_score.item<float>() << std::endl;

        fname = result_dir + '/' + std::get<1>(data).at(0);
        visualizer::save_image(output.detach(), fname, /*range=*/output_range, /*cols=*/1, /*padding=*/0);

    }

    // (6) Calculate Average
    ave_anomaly_score = ave_anomaly_score / (double)dataset.size();
    ave_res_loss = ave_res_loss / (double)dataset.size();
    ave_dis_loss = ave_dis_loss / (double)dataset.size();
    ave_time = ave_time / (double)dataset.size();

    // (7) Average Output
    std::cout << "<All> anomaly_score:" << ave_anomaly_score << " res:" << ave_res_loss << " dis:" << ave_dis_loss << " (time:" << ave_time << ')' << std::endl;
    ofs << "<All> anomaly_score:" << ave_anomaly_score << " res:" << ave_res_loss << " dis:" << ave_dis_loss << " (time:" << ave_time << ')' << std::endl;

    // Post Processing
    ofs.close();
    ofs_score.close();

    // End Processing
    return;

}


// ---------------------------------------------
// Function to Calculate Anomaly Score
// ---------------------------------------------
std::tuple<torch::Tensor, torch::Tensor, torch::Tensor> AnomalyScore(torch::Tensor image, torch::Tensor fake_image, GAN_Discriminator &dis, const float Lambda){
    torch::Tensor res_loss = torch::abs(image - fake_image).sum();
    torch::Tensor image_feature = dis->forward(image).second;
    torch::Tensor fake_image_feature = dis->forward(fake_image).second;
    torch::Tensor dis_loss = torch::abs(image_feature - fake_image_feature).sum();
    torch::Tensor anomaly_score = (1.0 - Lambda) * res_loss + Lambda * dis_loss;
    return {anomaly_score, res_loss, dis_loss};
}