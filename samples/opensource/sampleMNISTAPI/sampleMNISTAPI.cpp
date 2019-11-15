/*
 * Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//!
//! sampleMNISTAPI.cpp
//! This file contains the implementation of the MNIST API sample. It creates the network
//! for MNIST classification using the API.
//! It can be run with the following command line:
//! Command: ./sample_mnist_api [-h or --help] [-d=/path/to/data/dir or --datadir=/path/to/data/dir]
//! [--useDLACore=<int>]
//!

#include "argsParser.h"
#include "buffers.h"
#include "common.h"
#include "logger.h"

#include "NvCaffeParser.h"
#include "NvInfer.h"
#include <cuda_runtime_api.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

const std::string gSampleName = "TensorRT.sample_mnist_api";

//!
//! \brief The SampleMNISTAPIParams structure groups the additional parameters required by
//!         the SampleMNISTAPI sample.
//!
struct SampleMNISTAPIParams : public samplesCommon::SampleParams
{
    int inputH;                  //!< The input height
    int inputW;                  //!< The input width
    int outputSize;              //!< The output size
    std::string weightsFile;     //!< The filename of the weights file
    std::string mnistMeansProto; //!< The proto file containing means
};

//! \brief  The SampleMNISTAPI class implements the MNIST API sample
//!
//! \details It creates the network for MNIST classification using the API
//!
class SampleMNISTAPI
{
    template <typename T>
    using SampleUniquePtr = std::unique_ptr<T, samplesCommon::InferDeleter>;

public:
    SampleMNISTAPI(const SampleMNISTAPIParams& params)
        : mParams(params)
        , mEngine(nullptr)
    {
    }

    //!
    //! \brief Function builds the network engine
    //!
    bool build();

    //!
    //! \brief Runs the TensorRT inference engine for this sample
    //!
    bool infer();

    //!
    //! \brief Cleans up any state created in the sample class
    //!
    bool teardown();

private:
    SampleMNISTAPIParams mParams; //!< The parameters for the sample.

    int mNumber{0}; //!< The number to classify

    std::map<std::string, nvinfer1::Weights> mWeightMap; //!< The weight name to weight value map

    std::shared_ptr<nvinfer1::ICudaEngine> mEngine; //!< The TensorRT engine used to run the network

    //!
    //! \brief Uses the API to create the MNIST Network
    //!
    bool constructNetwork(SampleUniquePtr<nvinfer1::IBuilder>& builder,
        SampleUniquePtr<nvinfer1::INetworkDefinition>& network, SampleUniquePtr<nvinfer1::IBuilderConfig>& config);

    //!
    //! \brief Reads the input  and stores the result in a managed buffer
    //!
    bool processInput(const samplesCommon::BufferManager& buffers);

    //!
    //! \brief Classifies digits and verify result
    //!
    bool verifyOutput(const samplesCommon::BufferManager& buffers);

    //!
    //! \brief Loads weights from weights file
    //!
    std::map<std::string, nvinfer1::Weights> loadWeights(const std::string& file);
};

//!
//! \brief Creates the network, configures the builder and creates the network engine
//!
//! \details This function creates the MNIST network by using the API to create a model and builds
//!          the engine that will be used to run MNIST (mEngine)
//!
//! \return Returns true if the engine was created successfully and false otherwise
//!
bool SampleMNISTAPI::build()
{
    mWeightMap = loadWeights(locateFile(mParams.weightsFile, mParams.dataDirs));

    auto builder = SampleUniquePtr<nvinfer1::IBuilder>(nvinfer1::createInferBuilder(gLogger.getTRTLogger()));
    if (!builder)
    {
        return false;
    }

    auto network = SampleUniquePtr<nvinfer1::INetworkDefinition>(builder->createNetwork());
    if (!network)
    {
        return false;
    }

    auto config = SampleUniquePtr<nvinfer1::IBuilderConfig>(builder->createBuilderConfig());
    if (!config)
    {
        return false;
    }

    auto constructed = constructNetwork(builder, network, config);
    if (!constructed)
    {
        return false;
    }

    assert(network->getNbInputs() == 1);
    auto inputDims = network->getInput(0)->getDimensions();
    assert(inputDims.nbDims == 3);

    assert(network->getNbOutputs() == 1);
    auto outputDims = network->getOutput(0)->getDimensions();
    assert(outputDims.nbDims == 3);

    return true;
}

union Int8Config
{
    struct Layer {
        bool scale_1_use_int8;
        bool conv1_use_int8;
        bool pool1_use_int8;
        bool conv2_use_int8;
        bool pool2_use_int8;
        bool ip1_use_int8;
        bool relu1_use_int8;
        bool ip2_use_int8;
        bool prob_use_int8;
    } Layer;

    struct Bools {
        bool use_int8[9];
    } Bools;
};

union Int8Config gINT8_CONFIG;

void SetupAllLayerPrecision(INetworkDefinition* network, float scales) {
    auto num_layers = network->getNbLayers();
    for (int i = 0; i < num_layers; i++) {
        auto layer = network->getLayer(i);
        auto current_layer_use_int8 = gINT8_CONFIG.Bools.use_int8[i];
        gLogInfo << layer->getName() << " is set to " << ((current_layer_use_int8)?"kINT8":"kFLOAT") << std::endl;

        if (current_layer_use_int8) {
            layer->setPrecision(DataType::kINT8);
            auto input_tensor = layer->getInput(0);
            input_tensor->setDynamicRange(-scales, scales);
        }
        else {
            layer->setPrecision(DataType::kFLOAT);
            layer->setOutputType(0, DataType::kFLOAT);
        }

        int next_layer = i + 1;
        if (next_layer < num_layers) {
            auto next_layer_use_int8 = gINT8_CONFIG.Bools.use_int8[next_layer];
            if (next_layer_use_int8) {
                layer->setOutputType(0, DataType::kINT8);
            }
            else {
                layer->setOutputType(0, DataType::kFLOAT);
            }
        }
    }
}

void SetupDebugName(INetworkDefinition* network) {

    std::map<LayerType, std::string> ltype2str = {
        {LayerType::kCONVOLUTION, "Conv"},
        {LayerType::kFULLY_CONNECTED, "FC"},
        {LayerType::kACTIVATION, "ReLU"},
        {LayerType::kPOOLING, "Pool"},
        {LayerType::kSCALE, "Scale"},
        {LayerType::kSOFTMAX, "Softmax"}
    };

    auto num_layers = network->getNbLayers();
    for (int i = 0; i < num_layers; i++) {
        auto layer = network->getLayer(i);
        auto ltype = layer->getType();
        std::string layer_name = "Layer_" + std::to_string(i) + 
            "_" + ltype2str.at(ltype);
        layer->setName(layer_name.c_str());
    }

    for (int i = 0; i < num_layers; ++i) {
        auto layer = network->getLayer(i);
        std::string tensor_name = "Tensor_from_" + std::string(layer->getName());
        if (i + 1 < num_layers) {
            auto next_layer = network->getLayer(i + 1);
            tensor_name += "_to_" + std::string(next_layer->getName());
        }
        auto output_tenor = layer->getOutput(0);
        output_tenor->setName(tensor_name.c_str());
    }
}


//!
//! \brief Uses the API to create the MNIST Network
//!
//! \param network Pointer to the network that will be populated with the MNIST network
//!
//! \param builder Pointer to the engine builder
//!
bool SampleMNISTAPI::constructNetwork(SampleUniquePtr<nvinfer1::IBuilder>& builder,
    SampleUniquePtr<nvinfer1::INetworkDefinition>& network, SampleUniquePtr<nvinfer1::IBuilderConfig>& config)
{
    // Create input tensor of shape { 1, 1, 28, 28 }
    ITensor* data = network->addInput(
        mParams.inputTensorNames[0].c_str(), DataType::kFLOAT, Dims3{1, mParams.inputH, mParams.inputW});
    assert(data);
    data->setType(DataType::kFLOAT);

    // Create scale layer with default power/shift and specified scale parameter.
    const float scaleParam = 0.0125f;
    const Weights power{DataType::kFLOAT, nullptr, 0};
    const Weights shift{DataType::kFLOAT, nullptr, 0};
    const Weights scale{DataType::kFLOAT, &scaleParam, 1};
    IScaleLayer* scale_1 = network->addScale(*data, ScaleMode::kUNIFORM, shift, scale, power);
    assert(scale_1);

    // Add convolution layer with 20 outputs and a 5x5 filter.
    IConvolutionLayer* conv1 = network->addConvolution(
        *scale_1->getOutput(0), 20, DimsHW{5, 5}, mWeightMap["conv1filter"], mWeightMap["conv1bias"]);
    assert(conv1);
    conv1->setStride(DimsHW{1, 1});

    // Add max pooling layer with stride of 2x2 and kernel size of 2x2.
    IPoolingLayer* pool1 = network->addPooling(*conv1->getOutput(0), PoolingType::kMAX, DimsHW{2, 2});
    assert(pool1);
    pool1->setStride(DimsHW{2, 2});

    // Add second convolution layer with 50 outputs and a 5x5 filter.
    IConvolutionLayer* conv2 = network->addConvolution(
        *pool1->getOutput(0), 50, DimsHW{5, 5}, mWeightMap["conv2filter"], mWeightMap["conv2bias"]);
    assert(conv2);
    conv2->setStride(DimsHW{1, 1});

    // Add second max pooling layer with stride of 2x2 and kernel size of 2x3>
    IPoolingLayer* pool2 = network->addPooling(*conv2->getOutput(0), PoolingType::kMAX, DimsHW{2, 2});
    assert(pool2);
    pool2->setStride(DimsHW{2, 2});

    // Add fully connected layer with 500 outputs.
    IFullyConnectedLayer* ip1
        = network->addFullyConnected(*pool2->getOutput(0), 500, mWeightMap["ip1filter"], mWeightMap["ip1bias"]);
    assert(ip1);

    // Add activation layer using the ReLU algorithm.
    IActivationLayer* relu1 = network->addActivation(*ip1->getOutput(0), ActivationType::kRELU);
    assert(relu1);

    // Add second fully connected layer with 20 outputs.
    IFullyConnectedLayer* ip2 = network->addFullyConnected(
        *relu1->getOutput(0), mParams.outputSize, mWeightMap["ip2filter"], mWeightMap["ip2bias"]);
    assert(ip2);

    // Add softmax layer to determine the probability.
    ISoftMaxLayer* prob = network->addSoftMax(*ip2->getOutput(0));
    assert(prob);
    SetupDebugName(network.get());
    // over-write prob's name
    prob->getOutput(0)->setName(mParams.outputTensorNames[0].c_str());
    network->markOutput(*prob->getOutput(0));

    // Build engine
    builder->setMaxBatchSize(mParams.batchSize);
    config->setMaxWorkspaceSize(16_MiB);
    if (mParams.fp16)
    {
        config->setFlag(BuilderFlag::kFP16);
    }
    if (mParams.int8)
    {
        config->setFlag(BuilderFlag::kINT8);
        config->setFlag(BuilderFlag::kSTRICT_TYPES);
        SetupAllLayerPrecision(network.get(), 64.f);
    }

    samplesCommon::enableDLA(builder.get(), config.get(), mParams.dlaCore);

    mEngine = std::shared_ptr<nvinfer1::ICudaEngine>(
        builder->buildEngineWithConfig(*network, *config), samplesCommon::InferDeleter());
    if (!mEngine)
    {
        return false;
    }

    return true;
}

//!
//! \brief Runs the TensorRT inference engine for this sample
//!
//! \details This function is the main execution function of the sample. It allocates the buffer,
//!          sets inputs and executes the engine.
//!
bool SampleMNISTAPI::infer()
{
    // Create RAII buffer manager object
    samplesCommon::BufferManager buffers(mEngine, mParams.batchSize);

    auto context = SampleUniquePtr<nvinfer1::IExecutionContext>(mEngine->createExecutionContext());
    if (!context)
    {
        return false;
    }

    // Read the input data into the managed buffers
    assert(mParams.inputTensorNames.size() == 1);
    if (!processInput(buffers))
    {
        return false;
    }

    // Memcpy from host input buffers to device input buffers
    buffers.copyInputToDevice();

    bool status = context->execute(mParams.batchSize, buffers.getDeviceBindings().data());
    if (!status)
    {
        return false;
    }

    // Memcpy from device output buffers to host output buffers
    buffers.copyOutputToHost();

    // Verify results
    if (!verifyOutput(buffers))
    {
        return false;
    }

    return true;
}

//!
//! \brief Reads the input and stores the result in a managed buffer
//!
bool SampleMNISTAPI::processInput(const samplesCommon::BufferManager& buffers)
{
    // Read a random digit file
    srand(unsigned(time(nullptr)));
    std::vector<uint8_t> fileData(mParams.inputH * mParams.inputW);
    mNumber = rand() % mParams.outputSize;
    readPGMFile(locateFile(std::to_string(mNumber) + ".pgm", mParams.dataDirs), fileData.data(), mParams.inputH,
        mParams.inputW);

    // Print ASCII representation of digit image
    std::cout << "\nInput:\n" << std::endl;
    for (int i = 0; i < mParams.inputH * mParams.inputW; i++)
    {
        std::cout << (" .:-=+*#%@"[fileData[i] / 26]) << (((i + 1) % mParams.inputW) ? "" : "\n");
    }

    // Parse mean file
    auto parser = SampleUniquePtr<nvcaffeparser1::ICaffeParser>(nvcaffeparser1::createCaffeParser());
    if (!parser)
    {
        return false;
    }

    auto meanBlob = SampleUniquePtr<nvcaffeparser1::IBinaryProtoBlob>(
        parser->parseBinaryProto(locateFile(mParams.mnistMeansProto, mParams.dataDirs).c_str()));
    if (!meanBlob)
    {
        return false;
    }

    const float* meanData = reinterpret_cast<const float*>(meanBlob->getData());
    if (!meanData)
    {
        return false;
    }

    // Subtract mean from image
    float* hostDataBuffer = static_cast<float*>(buffers.getHostBuffer(mParams.inputTensorNames[0]));
    for (int i = 0; i < mParams.inputH * mParams.inputW; i++)
    {
        hostDataBuffer[i] = float(fileData[i]) - meanData[i];
    }

    return true;
}

//!
//! \brief Classifies digits and verify result
//!
//! \return whether the classification output matches expectations
//!
bool SampleMNISTAPI::verifyOutput(const samplesCommon::BufferManager& buffers)
{
    float* prob = static_cast<float*>(buffers.getHostBuffer(mParams.outputTensorNames[0]));
    std::cout << "\nOutput:\n" << std::endl;
    float maxVal{0.0f};
    int idx{0};
    for (int i = 0; i < mParams.outputSize; i++)
    {
        if (maxVal < prob[i])
        {
            maxVal = prob[i];
            idx = i;
        }
        std::cout << i << ": " << std::string(int(std::floor(prob[i] * 10 + 0.5f)), '*') << std::endl;
    }
    std::cout << std::endl;

    return idx == mNumber && maxVal > 0.9f;
}

//!
//! \brief Cleans up any state created in the sample class
//!
bool SampleMNISTAPI::teardown()
{
    // Release weights host memory
    for (auto& mem : mWeightMap)
    {
        auto weight = mem.second;
        if (weight.type == DataType::kFLOAT)
        {
            delete[] static_cast<const uint32_t*>(weight.values);
        }
        else
        {
            delete[] static_cast<const uint16_t*>(weight.values);
        }
    }

    return true;
}

//!
//! \brief Loads weights from weights file
//!
//! \details TensorRT weight files have a simple space delimited format
//!          [type] [size] <data x size in hex>
//!
std::map<std::string, nvinfer1::Weights> SampleMNISTAPI::loadWeights(const std::string& file)
{
    gLogInfo << "Loading weights: " << file << std::endl;

    // Open weights file
    std::ifstream input(file, std::ios::binary);
    assert(input.is_open() && "Unable to load weight file.");

    // Read number of weight blobs
    int32_t count;
    input >> count;
    assert(count > 0 && "Invalid weight map file.");

    std::map<std::string, nvinfer1::Weights> weightMap;
    while (count--)
    {
        nvinfer1::Weights wt{DataType::kFLOAT, nullptr, 0};
        int type;
        uint32_t size;

        // Read name and type of blob
        std::string name;
        input >> name >> std::dec >> type >> size;
        wt.type = static_cast<DataType>(type);

        // Load blob
        if (wt.type == DataType::kFLOAT)
        {
            uint32_t* val = new uint32_t[size];
            for (uint32_t x = 0; x < size; ++x)
            {
                input >> std::hex >> val[x];
            }
            wt.values = val;
        }
        else if (wt.type == DataType::kHALF)
        {
            uint16_t* val = new uint16_t[size];
            for (uint32_t x = 0; x < size; ++x)
            {
                input >> std::hex >> val[x];
            }
            wt.values = val;
        }

        wt.count = size;
        weightMap[name] = wt;
    }

    return weightMap;
}

//!
//! \brief Initializes members of the params struct using the command line args
//!
SampleMNISTAPIParams initializeSampleParams(const samplesCommon::Args& args)
{
    SampleMNISTAPIParams params;
    if (args.dataDirs.empty()) //!< Use default directories if user hasn't provided directory paths
    {
        params.dataDirs.push_back("data/mnist/");
        params.dataDirs.push_back("data/samples/mnist/");
    }
    else //!< Use the data directory provided by the user
    {
        params.dataDirs = args.dataDirs;
    }
    params.inputTensorNames.push_back("data");
    params.batchSize = 1;
    params.outputTensorNames.push_back("prob");
    params.dlaCore = args.useDLACore;
    params.int8 = args.runInInt8;
    params.fp16 = args.runInFp16;

    params.inputH = 28;
    params.inputW = 28;
    params.outputSize = 10;
    params.weightsFile = "mnistapi.wts";
    params.mnistMeansProto = "mnist_mean.binaryproto";

    return params;
}

//!
//! \brief Prints the help information for running this sample
//!
void printHelpInfo()
{
    std::cout
        << "Usage: ./sample_mnist_api [-h or --help] [-d or --datadir=<path to data directory>] [--useDLACore=<int>]"
        << std::endl;
    std::cout << "--help          Display help information" << std::endl;
    std::cout << "--datadir       Specify path to a data directory, overriding the default. This option can be used "
                 "multiple times to add multiple directories. If no data directories are given, the default is to use "
                 "(data/samples/mnist/, data/mnist/)"
              << std::endl;
    std::cout << "--useDLACore=N  Specify a DLA engine for layers that support DLA. Value can range from 0 to n-1, "
                 "where n is the number of DLA engines on the platform."
              << std::endl;
    std::cout << "--int8          Run in Int8 mode." << std::endl;
    std::cout << "--fp16          Run in FP16 mode." << std::endl;
}

int main(int argc, char** argv)
{
    for (int i = 0; i < 9; ++i) {
        gINT8_CONFIG.Bools.use_int8[i] = false;
    }

    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "l") == 0) {
                if (i + 1 >= argc) {
                    gLogError << ("need argument for \"l\"") << std::endl;
                    return 0;
                }
                int layer = std::atoi(argv[i + 1]);
                if (layer < 0 || layer >=9) {
                    gLogError << ("\"l\" argument must be 0~8") << std::endl;
                    return 0;
                }
                gINT8_CONFIG.Bools.use_int8[layer] = true;
            }
        }
    }

    samplesCommon::Args args;
    bool argsOK = samplesCommon::parseArgs(args, argc, argv);
    if (!argsOK)
    {
        gLogError << "Invalid arguments" << std::endl;
        printHelpInfo();
        return EXIT_FAILURE;
    }
    if (args.help)
    {
        printHelpInfo();
        return EXIT_SUCCESS;
    }

    auto sampleTest = gLogger.defineTest(gSampleName, argc, argv);

    gLogger.reportTestStart(sampleTest);

    SampleMNISTAPI sample(initializeSampleParams(args));

    gLogInfo << "Building and running a GPU inference engine for MNIST API" << std::endl;

    if (!sample.build())
    {
        return gLogger.reportFail(sampleTest);
    }
    if (!sample.infer())
    {
        return gLogger.reportFail(sampleTest);
    }
    if (!sample.teardown())
    {
        return gLogger.reportFail(sampleTest);
    }

    return gLogger.reportPass(sampleTest);
}
