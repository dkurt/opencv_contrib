/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                           License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2013, OpenCV Foundation, all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include "../precomp.hpp"
#include <limits>
#include <set>
#include <map>
#include <algorithm>
#include <iostream>
#include <fstream>

namespace cv {
namespace dnn {
#if defined(ENABLE_TORCH_IMPORTER) && ENABLE_TORCH_IMPORTER
#include "THDiskFile.h"

//#ifdef NDEBUG
static bool dbgPrint = false;
//#else
//static bool dbgPrint = true;
//#endif

enum LuaType
{
    TYPE_NIL      = 0,
    TYPE_NUMBER   = 1,
    TYPE_STRING   = 2,
    TYPE_TABLE    = 3,
    TYPE_TORCH    = 4,
    TYPE_BOOLEAN  = 5,
    TYPE_FUNCTION = 6,
    TYPE_RECUR_FUNCTION = 8,
    LEGACY_TYPE_RECUR_FUNCTION = 7
};

template<typename T>
static String toString(const T &v)
{
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

static inline bool startsWith(const String &str, const char *substr)
{
    return str.find(substr) == 0;
}

static inline bool endsWith(const String &str, const char *substr)
{
    return str.rfind(substr) == str.length() - strlen(substr);
}

struct TorchImporter : public ::cv::dnn::Importer
{
    typedef std::map<String, std::pair<int, Mat> > TensorsMap;
    Net net;

    THFile *file;
    std::set<int> readedIndexes;
    std::map<int, Mat> storages;
    std::map<int, Mat> tensors;

    struct Module
    {
        String thName, apiType;
        dnn::LayerParams params;
        std::vector<cv::Ptr<Module> > modules;

        Module(const String &_thName, const String &_apiType = String())
            : thName(_thName), apiType(_apiType) {}
    };

    Module *rootModule;
    Module *curModule;
    int moduleCounter;

    TorchImporter(String filename, bool isBinary)
    {
        rootModule = curModule = NULL;
        moduleCounter = 0;

        file = THDiskFile_new(filename.c_str(), "r", 0);
        CV_Assert(file && THFile_isOpened(file));

        if (isBinary)
            THFile_binary(file);
        else
            THFile_ascii(file);
    }

    /* Simple readers */

    inline int readInt()
    {
        return THFile_readIntScalar(file);
    }

    inline long readLong()
    {
        return THFile_readLongScalar(file);
    }

    inline bool readBool()
    {
        return readInt() != 0;
    }

    inline double readDouble()
    {
        return THFile_readDoubleScalar(file);
    }

    inline String readString()
    {
        int size = THFile_readIntScalar(file);
        String str(size, '\0');
        THFile_readCharRaw(file, const_cast<char*>(str.c_str()), size);
        return str;
    }

    inline String readTorchClassName()
    {
        String version = readString();
        return startsWith(version, "V ") ? readString() : version;
    }

    inline void readFunction()
    {
        readString();
        readObject();
    }

    void readTable(int index = -1)
    {
        index = (index < 0) ? readInt() : index;

        if (readedIndexes.count(index))
            return;

        readedIndexes.insert(index);

        int size = readInt();

        for (int i = 0; i < size; i++)
        {
            readObject(); //key
            readObject(); //value
        }
    }

    /* Special readers */

    static inline int parseTorchType(const String &str, const char *suffix, const char *prefix = "torch.")
    {
        if (startsWith(str, prefix) && endsWith(str, suffix))
        {
           String typeStr = str.substr(strlen(prefix), str.length() - strlen(prefix) - strlen(suffix));

           if (typeStr == "Double")
               return CV_64F;
           else if (typeStr == "Float" || typeStr == "Cuda")
               return CV_32F;
           else if (typeStr == "Byte")
               return CV_8U;
           else if (typeStr == "Char")
               return CV_8S;
           else if (typeStr == "Short")
               return CV_16S;
           else if (typeStr == "Int")
               return CV_32S;
           else if (typeStr == "Long") //Carefully! CV_64S type coded as CV_USRTYPE1
               return CV_USRTYPE1;
           else
               CV_Error(Error::StsNotImplemented, "Unknown type \"" + typeStr + "\" of torch class \"" + str + "\"");
        }

        return -1;
    }

    static int parseTensorType(const String &className)
    {
        return parseTorchType(className, "Tensor");
    }

    static int parseStorageType(const String &className)
    {
        return parseTorchType(className, "Storage");
    }

    void readTorchStorage(int index, int type = -1)
    {
        long size = readLong();
        Mat storageMat(1, size, (type != CV_USRTYPE1) ? type : CV_64F); //handle LongStorage as CV_64F Mat

        switch (type)
        {
        case CV_32F:
            THFile_readFloatRaw(file, (float*)storageMat.data, size);
            break;
        case CV_64F:
            THFile_readDoubleRaw(file, (double*)storageMat.data, size);
            break;
        case CV_8S:
        case CV_8U:
            THFile_readByteRaw(file, (uchar*)storageMat.data, size);
            break;
        case CV_16S:
        case CV_16U:
            THFile_readShortRaw(file, (short*)storageMat.data, size);
            break;
        case CV_32S:
            THFile_readIntRaw(file, (int*)storageMat.data, size);
            break;
        case CV_USRTYPE1:
        {
            double *buf = storageMat.ptr<double>();
            THFile_readLongRaw(file, (long*)buf, size);

            for (size_t i = (size_t)size; i-- > 0; )
                buf[i] = ((long*)buf)[i];
        }
            break;
        default:
            CV_Error(Error::StsInternal, "");
            break;
        }

        storages.insert(std::make_pair(index, storageMat));
    }

    void readTorchTable(Dict &scalarParams, TensorsMap &tensorParams)
    {
        int luaType = readInt();
        int index = readInt();

        CV_Assert(luaType == TYPE_TABLE && readedIndexes.count(index) == 0);
        readedIndexes.insert(index);

        long fpos;
        int numPairs = readInt();

        for (int i = 0; i < numPairs; i++)
        {
            fpos = THFile_position(file);
            int ktype = readInt();

            if (ktype != TYPE_STRING) //skip non-string fileds
            {
                THFile_seek(file, fpos);
                readObject(); //key
                readObject(); //value
                continue;
            }

            String key = readString();
            if (dbgPrint)
                std::cout << i << "th key: " << key << "\n";

            fpos = THFile_position(file);
            int vtype = readInt();

            if (vtype == TYPE_TORCH)
            {
                int index = readInt();
                readTorchObject(index);

                if (tensors.count(index)) //tensor was readed
                {
                    tensorParams.insert(std::make_pair(key, std::make_pair(index, tensors[index])));
                }
                else if (storages.count(index)) //storage was readed
                {
                    Mat &matStorage = storages[index];
                    Mat matCasted;
                    matStorage.convertTo(matCasted, CV_64F);

                    DictValue scalar = DictValue::arrayReal(matCasted.ptr<double>(), matCasted.total());
                    scalarParams.set(key, scalar);
                }
            }
            else if (vtype == TYPE_NUMBER)
            {
                scalarParams.set(key, readDouble());
            }
            else if (vtype == TYPE_STRING)
            {
                scalarParams.set(key, readString());
            }
            else if (vtype == TYPE_BOOLEAN)
            {
                scalarParams.set(key, readBool());
            }
            else
            {
                THFile_seek(file, fpos);
                readObject();
            }
        }

        //Debug output
        if (dbgPrint)
        {
            std::cout << "scalarParams:\n";
            std::cout << scalarParams;

            std::cout << "#" << tensorParams.size() << " tensorParams:\n";
            std::map<String,std::pair<int, Mat> >::const_iterator it;
            for (it = tensorParams.begin(); it != tensorParams.end(); it++)
                std::cout << it->first << ": Tensor " << it->second.second.size << "\n";
        }
    }

    void readTorchTensor(int indexTensor, int typeTensor)
    {
        int ndims = readInt();
        AutoBuffer<long, 4> sizes(ndims);
        AutoBuffer<long, 4> steps(ndims);
        THFile_readLongRaw(file, sizes, ndims);
        THFile_readLongRaw(file, steps, ndims);
        long offset = readLong() - 1;

        //read Storage
        int typeidx = readInt();
        CV_Assert(typeidx == TYPE_TORCH || (typeidx == TYPE_NIL && ndims == 0));

        if (typeidx == TYPE_NIL)
        {
            tensors.insert(std::make_pair(indexTensor, Mat()));
            return;
        }

        int indexStorage = readInt();
        if (readedIndexes.count(indexStorage) == 0)
        {
            String className = readTorchClassName();
            int typeStorage = parseStorageType(className);
            CV_Assert(typeStorage >= 0 && typeTensor == typeStorage);
            readTorchStorage(indexStorage, typeStorage);
            typeTensor = storages[indexStorage].type();
            readedIndexes.insert(indexStorage);
        }

        //small check
        size_t requireElems = (size_t)offset + (size_t)steps[0] * (size_t)sizes[0];
        size_t storageElems = storages[indexStorage].total();
        if (requireElems > storageElems)
            CV_Error(Error::StsBadSize, "Storage has insufficent number of elemements for requested Tensor");

        //convert sizes
        AutoBuffer<int, 4> isizes(ndims);
        AutoBuffer<size_t, 4> ssteps(ndims);
        for (int i = ndims - 1; i >= 0; i--)
        {
            isizes[i] = (int)sizes[i];
            ssteps[i] = (size_t)steps[i] * CV_ELEM_SIZE(typeTensor);
        }

        //allocate Blob
        Mat srcMat(ndims, (int*)isizes, typeTensor , storages[indexStorage].ptr() + offset*CV_ELEM_SIZE(typeTensor), (size_t*)ssteps);
        int dstType = CV_32F;

        Mat blob;
        srcMat.convertTo(blob, dstType);

        tensors.insert(std::make_pair(indexTensor, blob));
    }

    static bool isNNClass(const String &className, String &nnName)
    {
        const char *prefixes[] = {"nn.", "cunn.", "cudnn.", "fbcunn.", NULL};

        for (int i = 0; prefixes[i]; i++)
        {
            if (startsWith(className, prefixes[i]))
            {
                nnName = className.substr(strlen(prefixes[i]));
                return true;
            }
        }

        return false;
    }

    static void convertTorchKernelsParams(const Dict &torchParams, cv::dnn::LayerParams &layerParams)
    {
        layerParams.set("kernel_h", torchParams.get<int>("kH"));
        layerParams.set("kernel_w", torchParams.get<int>("kW"));
        layerParams.set("stride_h", torchParams.get<int>("dH"));
        layerParams.set("stride_w", torchParams.get<int>("dW"));
        layerParams.set("pad_h", torchParams.get<int>("padH", 0));
        layerParams.set("pad_w", torchParams.get<int>("padW", 0));
    }

    void readTorchObject(int index)
    {
        if(readedIndexes.count(index))
            return;

        String className = readTorchClassName();
        String nnName;

        if (dbgPrint)
            std::cout << "Class: " << className << std::endl;

        int type;
        if ( (type = parseTensorType(className)) >= 0 ) //is Tensor
        {
            readTorchTensor(index, type);
        }
        else if ( (type = parseStorageType(className)) >= 0 ) //is Storage
        {
            readTorchStorage(index, type);
        }
        else if (isNNClass(className, nnName))
        {
            Dict scalarParams;
            TensorsMap tensorParams;

            cv::Ptr<Module> newModule(new Module(nnName));
            cv::dnn::LayerParams &layerParams = newModule->params;

            layerParams.set("torch_index", index);

            if (nnName == "Sequential" || nnName == "Parallel" ||
                    nnName == "Concat" || nnName == "ConcatTable" || nnName == "JoinTable")
            {
                Module *parentModule = curModule;
                curModule->modules.push_back(newModule);
                curModule = newModule;
                readTorchTable(scalarParams, tensorParams);
                curModule = parentModule;

                if (nnName == "Parallel")
                {
                    layerParams.set("inputDimension", scalarParams.get<int>("inputDimension"));
                    layerParams.set("outputDimension", scalarParams.get<int>("outputDimension"));
                }
                if (nnName == "Concat")
                {
                    layerParams.set("dimension", scalarParams.get<int>("dimension"));
                }
                if (nnName == "JoinTable")
                {
                    layerParams.set("dimension", scalarParams.get<int>("dimension"));
                }
            }
            else if (nnName == "SpatialConvolution")
            {
                newModule->apiType = "Convolution";
                readTorchTable(scalarParams, tensorParams);

                CV_Assert(tensorParams.count("weight"));
                layerParams.blobs.push_back(tensorParams["weight"].second);

                bool bias = tensorParams.count("bias") != 0;
                layerParams.set("bias_term", bias);
                if (bias)
                    layerParams.blobs.push_back(tensorParams["bias"].second);

                layerParams.set("num_output", scalarParams.get<int>("nOutputPlane"));
                convertTorchKernelsParams(scalarParams, layerParams);

                curModule->modules.push_back(newModule);
            }
            else if (nnName == "SpatialMaxPooling" || nnName == "SpatialAveragePooling")
            {
                newModule->apiType = "Pooling";
                readTorchTable(scalarParams, tensorParams);

                if (nnName == "SpatialMaxPooling") {
                    layerParams.set("pool", "MAX");
                    layerParams.set("indices_blob_id", tensorParams["indices"].first);
                }
                if (nnName == "SpatialAveragePooling")
                    layerParams.set("pool", "AVE");
                convertTorchKernelsParams(scalarParams, layerParams);

                curModule->modules.push_back(newModule);
            }
            else if (nnName == "Linear")
            {
                newModule->apiType = "InnerProduct";
                readTorchTable(scalarParams, tensorParams);

                CV_Assert(tensorParams.count("weight"));
                Mat weightBlob = tensorParams["weight"].second;
                layerParams.blobs.push_back(weightBlob);

                bool bias = tensorParams.count("bias") != 0;
                if (bias)
                    layerParams.blobs.push_back(tensorParams["bias"].second);
                layerParams.set("bias_term", bias);

                layerParams.set("num_output", weightBlob.size[0]);
                curModule->modules.push_back(newModule);
            }
            else if (nnName == "Reshape")
            {
                newModule->apiType = "Reshape";

                readTorchTable(scalarParams, tensorParams);
                CV_Assert(scalarParams.has("size"));

                DictValue dimParam = scalarParams.get("size");
                layerParams.set("dim", dimParam);

                if (scalarParams.has("batchMode") && scalarParams.get<bool>("batchMode"))
                    layerParams.set("axis", 1);

                curModule->modules.push_back(newModule);
            }
            else if (nnName == "ReLU")
            {
                curModule->modules.push_back(cv::Ptr<Module>(new Module(nnName, "ReLU")));
                readObject();
            }
            else if (nnName == "Tanh")
            {
                curModule->modules.push_back(cv::Ptr<Module>(new Module(nnName, "TanH")));
                readObject();
            }
            else if (nnName == "Sigmoid")
            {
                curModule->modules.push_back(cv::Ptr<Module>(new Module(nnName, "Sigmoid")));
                readObject();
            }
            else if (nnName == "SpatialBatchNormalization")
            {
                newModule->apiType = "BatchNorm";
                readTorchTable(scalarParams, tensorParams);

                CV_Assert(tensorParams.count("running_var") &&
                          tensorParams.count("running_mean"));
                layerParams.blobs.push_back(tensorParams["running_mean"].second);
                layerParams.blobs.push_back(tensorParams["running_var"].second);

                CV_Assert(scalarParams.has("eps"));
                float eps = float(scalarParams.get<double>("eps"));
                layerParams.set("eps", eps);

                if (tensorParams.count("weight"))
                {
                    layerParams.set("has_weight", true);
                    layerParams.blobs.push_back(tensorParams["weight"].second);
                }

                if (tensorParams.count("bias"))
                {
                    layerParams.set("has_bias", true);
                    layerParams.blobs.push_back(tensorParams["bias"].second);
                }

                curModule->modules.push_back(newModule);
            }
            else if (nnName == "PReLU")
            {
                readTorchTable(scalarParams, tensorParams);

                CV_Assert(tensorParams.count("weight"));

                size_t outputChannels = static_cast<int>(scalarParams.get<double>("nOutputPlane"));
                if (outputChannels) {

                    CV_Assert(tensorParams["weight"].second.total() == outputChannels);
                    layerParams.blobs.push_back(tensorParams["weight"].second);

                    newModule->apiType = "ChannelsPReLU";
                }
                else {
                    CV_Assert(tensorParams["weight"].second.total() == 1);
                    float negative_slope = *tensorParams["weight"].second.ptr<float>();
                    layerParams.set("negative_slope", negative_slope);

                    newModule->apiType = "ReLU";
                }

                curModule->modules.push_back(newModule);
            }
            else if (nnName == "SpatialDropout")
            {
                readTorchTable(scalarParams, tensorParams);
                CV_Assert(scalarParams.has("p"));

                float scale = 1 -  scalarParams.get<double>("p");

                CV_Assert(scale > 0);

                newModule->apiType = "Power";
                layerParams.set("scale", scale);
                curModule->modules.push_back(newModule);
            }
            else if (nnName == "Identity")
            {
                readTorchTable(scalarParams, tensorParams);
                newModule->apiType = "Identity";
                curModule->modules.push_back(newModule);
            }
            else if (nnName == "Padding")
            {
                readTorchTable(scalarParams, tensorParams);
                newModule->apiType = "Padding";

                CV_Assert(scalarParams.has("pad") &&
                          scalarParams.has("dim"));

                layerParams.set("padding_dim",
                                static_cast<int>(scalarParams.get<double>("dim") - 1));
                layerParams.set("padding", static_cast<int>(scalarParams.get<double>("pad")));

                if (scalarParams.has("nInputDim"))
                    layerParams.set("input_dims",
                                    static_cast<int>(scalarParams.get<double>("nInputDim")));

                if (scalarParams.has("value"))
                    layerParams.set("value", scalarParams.get<double>("value"));

                if (scalarParams.has("index"))
                    layerParams.set("index",
                                    static_cast<int>(scalarParams.get<double>("index") - 1));

                curModule->modules.push_back(newModule);
            }
            else if (nnName == "CAddTable")
            {
                curModule->modules.push_back(newModule);
                readObject();
            }
            else if (nnName == "SpatialDilatedConvolution")
            {
                readTorchTable(scalarParams, tensorParams);
                newModule->apiType = "Convolution";
                CV_Assert(scalarParams.has("padW") &&
                          scalarParams.has("padH")&&
                          scalarParams.has("dW")&&
                          scalarParams.has("dH")&&
                          scalarParams.has("dilationW")&&
                          scalarParams.has("dilationH")&&
                          scalarParams.has("kW")&&
                          scalarParams.has("kH")&&
                          scalarParams.has("nOutputPlane"));

                layerParams.set("kernel_w", static_cast<int>(scalarParams.get<double>("kW")));
                layerParams.set("kernel_h", static_cast<int>(scalarParams.get<double>("kH")));
                layerParams.set("pad_w", static_cast<int>(scalarParams.get<double>("padW")));
                layerParams.set("pad_h", static_cast<int>(scalarParams.get<double>("padH")));
                layerParams.set("stride_w", static_cast<int>(scalarParams.get<double>("dW")));
                layerParams.set("stride_h", static_cast<int>(scalarParams.get<double>("dH")));
                layerParams.set("dilation_w", static_cast<int>(scalarParams.get<double>("dilationW")));
                layerParams.set("dilation_h", static_cast<int>(scalarParams.get<double>("dilationH")));
                layerParams.set("num_output", static_cast<int>(scalarParams.get<double>("nOutputPlane")));

                layerParams.blobs.push_back(tensorParams["weight"].second);

                bool bias = tensorParams.count("bias");
                layerParams.set("bias_term", bias);
                if (bias)
                    layerParams.blobs.push_back(tensorParams["bias"].second);

                curModule->modules.push_back(newModule);
            }
            else if (nnName == "SpatialFullConvolution")
            {
                readTorchTable(scalarParams, tensorParams);
                newModule->apiType = "Deconvolution";
                CV_Assert(scalarParams.has("padW") &&
                          scalarParams.has("padH")&&
                          scalarParams.has("dW")&&
                          scalarParams.has("dH")&&
                          scalarParams.has("adjW")&&
                          scalarParams.has("adjH")&&
                          scalarParams.has("kW")&&
                          scalarParams.has("kH")&&
                          scalarParams.has("nOutputPlane"));

                layerParams.set("kernel_w", static_cast<int>(scalarParams.get<double>("kW")));
                layerParams.set("kernel_h", static_cast<int>(scalarParams.get<double>("kH")));
                layerParams.set("pad_w", static_cast<int>(scalarParams.get<double>("padW")));
                layerParams.set("pad_h", static_cast<int>(scalarParams.get<double>("padH")));
                layerParams.set("stride_w", static_cast<int>(scalarParams.get<double>("dW")));
                layerParams.set("stride_h", static_cast<int>(scalarParams.get<double>("dH")));
                layerParams.set("adj_w", static_cast<int>(scalarParams.get<double>("adjW")));
                layerParams.set("adj_h", static_cast<int>(scalarParams.get<double>("adjH")));
                layerParams.set("num_output", static_cast<int>(scalarParams.get<double>("nOutputPlane")));

                Mat weights = tensorParams["weight"].second;
                CV_Assert(weights.dims == 4);
                int reorderedShape[] = { weights.size[1], weights.size[0], weights.size[2], weights.size[3] };
                layerParams.blobs.push_back(weights.reshape(1, 4, reorderedShape));

                bool bias = tensorParams.count("bias");
                layerParams.set("bias_term", bias);
                if (bias)
                    layerParams.blobs.push_back(tensorParams["bias"].second);

                curModule->modules.push_back(newModule);
            }
            else if (nnName == "SpatialMaxUnpooling")
            {
                readTorchTable(scalarParams, tensorParams);
                CV_Assert(tensorParams.count("indices"));

                layerParams.set("indices_blob_id", tensorParams["indices"].first);
                curModule->modules.push_back(newModule);
            }
            else
            {
                CV_Error(Error::StsNotImplemented, "Unknown nn class \"" + className + "\"");
            }
        }
        else
        {
            CV_Error(Error::StsNotImplemented, "Unsupported Torch class \"" + className + "\"");
        }

        readedIndexes.insert(index);
    }

    void readObject()
    {
        int typeidx = readInt();

        if (typeidx == TYPE_TORCH)
        {
            int index = readInt();
            readTorchObject(index);
            readedIndexes.insert(index);
        }
        else if (typeidx == TYPE_NIL)
            return;
        else if (typeidx == TYPE_NUMBER)
            readDouble();
        else if (typeidx == TYPE_BOOLEAN)
            readBool();
        else if (typeidx == TYPE_STRING)
            readString();
        else if (typeidx == TYPE_TABLE)
            readTable();
        else
            CV_Error(Error::StsNotImplemented, "Unsupported Lua type");
    }

    inline String generateLayerName(const String &label = String())
    {
        return "l" + toString(++this->moduleCounter) + "_" + label;
    }

    int fill(Module *module, std::vector<std::pair<int, Module*> >& addedModules, int prevLayerId = 0, int prevOutNum = 0)
    {
        if (module == NULL)
            return prevLayerId;

        if (module->apiType.length())
        {
            int newLayerId = net.addLayer(generateLayerName(module->apiType), module->apiType, module->params);
            net.connect(prevLayerId, prevOutNum, newLayerId, 0);
            addedModules.push_back(std::make_pair(newLayerId, module));
            return newLayerId;
        }
        else
        {
            if (module->thName == "Sequential")
            {
                for (size_t i = 0; i < module->modules.size(); i++)
                {
                    prevLayerId = fill(module->modules[i], addedModules, prevLayerId, prevOutNum);
                    prevOutNum = 0;
                }
                return prevLayerId;
            }
            else if (module->thName == "Concat")
            {
                int newId, splitId, mergeId;
                LayerParams mergeParams, splitParams;
                mergeParams.set("axis", module->params.get<int>("dimension") - 1);

                splitId = net.addLayer(generateLayerName("torchSplit"), "Split", splitParams);
                mergeId = net.addLayer(generateLayerName("torchMerge"), "Concat", mergeParams);
                net.connect(prevLayerId, prevOutNum, splitId, 0);

                for (int i = 0; i < (int)module->modules.size(); i++)
                {
                    newId = fill(module->modules[i], addedModules, splitId, i);
                    net.connect(newId, 0, mergeId, i);
                }

                addedModules.push_back(std::make_pair(mergeId, module));
                return mergeId;
            }
            else if (module->thName == "Parallel")
            {
                int newId, splitId, mergeId, reshapeId;

                LayerParams splitParams, mergeParams, reshapeParams;
                splitParams.set("axis", module->params.get<int>("inputDimension") - 1);
                mergeParams.set("axis", module->params.get<int>("outputDimension") - 1);
                reshapeParams.set("axis", splitParams.get<int>("axis"));
                reshapeParams.set("num_axes", 1);

                splitId = net.addLayer(generateLayerName("torchSplit"), "Slice", splitParams);
                mergeId = net.addLayer(generateLayerName("torchMerge"), "Concat", mergeParams);
                reshapeId = net.addLayer(generateLayerName("torchReshape"), "Reshape", reshapeParams);
                net.connect(prevLayerId, prevOutNum, splitId, 0);

                for (int i = 0; i < (int)module->modules.size(); i++)
                {
                    net.connect(splitId, i, reshapeId, i);
                    newId = fill(module->modules[i], addedModules, reshapeId, i);
                    net.connect(newId, 0, mergeId, i);
                }

                addedModules.push_back(std::make_pair(mergeId, module));
                return mergeId;
            }
            else if (module->thName == "ConcatTable") {
                int newId, splitId;
                LayerParams splitParams;

                splitId = net.addLayer(generateLayerName("torchSplit"), "Split", splitParams);
                net.connect(prevLayerId, prevOutNum, splitId, 0);

                addedModules.push_back(std::make_pair(splitId, module));

                for (int i = 0; i < (int)module->modules.size(); i++)
                {
                    newId = fill(module->modules[i], addedModules, splitId, i);
                }

                return newId;
            }
            else if (module->thName == "JoinTable") {
                std::vector<int> ids = net.getUnconnectedOutLayers();

                int mergeId;
                LayerParams mergeParams;
                mergeParams.set("axis", module->params.get<int>("dimension") - 1);

                mergeId = net.addLayer(generateLayerName("torchMerge"), "Concat", mergeParams);
                addedModules.push_back(std::make_pair(mergeId, module));

                for (int i = 0; i < ids.size(); i++)
                {
                    net.connect(ids[i], 0, mergeId, i);
                }

                return mergeId;
            }
            else if (module->thName == "CAddTable") {
                String name = generateLayerName("torchCAddTable");
                std::vector<int> ids = net.getUnconnectedOutLayers();
                LayerParams params;
                params.set("operation", "sum");


                int id = net.addLayer(name, "Eltwise", params);

                for (int i = 0; i < ids.size(); i++)
                {
                    net.connect(ids[i], 0, id, i);
                }

                addedModules.push_back(std::make_pair(id, module));
                return id;
            }
            else if (module->thName == "SpatialMaxUnpooling") {
                CV_Assert(module->params.has("indices_blob_id"));
                int indicesBlobId = module->params.get<int>("indices_blob_id");
                std::pair<int, Module*> poolingLayer;
                poolingLayer.first = -1;

                for(int i = 0; i < addedModules.size(); i++)
                {
                    if (addedModules[i].second->apiType == "Pooling" &&
                        addedModules[i].second->params.has("indices_blob_id") &&
                        addedModules[i].second->params.get<int>("indices_blob_id") == indicesBlobId)
                    {
                        poolingLayer = addedModules[i];
                        break;
                    }
                }

                module->params.set("pool_k_h", poolingLayer.second->params.get<int>("kernel_h"));
                module->params.set("pool_k_w", poolingLayer.second->params.get<int>("kernel_w"));
                module->params.set("pool_stride_h", poolingLayer.second->params.get<int>("stride_h"));
                module->params.set("pool_stride_w", poolingLayer.second->params.get<int>("stride_w"));
                module->params.set("pool_pad_h", poolingLayer.second->params.get<int>("pad_h"));
                module->params.set("pool_pad_w", poolingLayer.second->params.get<int>("pad_w"));

                String name = generateLayerName("torchMaxUnpooling");
                int id = net.addLayer(name, "MaxUnpool", module->params);
                net.connect(prevLayerId, 0, id, 0);

                CV_Assert(poolingLayer.first != -1);
                net.connect(poolingLayer.first, 1, id, 1);

                return id;
            }
        }

        CV_Error(Error::StsInternal, "Unexpected torch container: " + module->thName);
        return -1;
    }

    void populateNet(Net net_)
    {
        if (rootModule == NULL)
        {
            rootModule = new Module("Sequential");
            curModule = rootModule;

            THFile_seek(file, 0);
            readObject();
        }

        net = net_;
        std::vector<std::pair<int, Module*> > addedModules;
        fill(rootModule, addedModules);
    }
};

Ptr<Importer> createTorchImporter(const String &filename, bool isBinary)
{
    return Ptr<Importer>(new TorchImporter(filename, isBinary));
}


Mat readTorchBlob(const String &filename, bool isBinary)
{
    Ptr<TorchImporter> importer(new TorchImporter(filename, isBinary));
    importer->readObject();
    CV_Assert(importer->tensors.size() == 1);

    return importer->tensors.begin()->second;
}
#else

Ptr<Importer> createTorchImporter(const String &filename, bool isBinary)
{
    CV_Error(Error::StsNotImplemented, "Torch importer is disabled in current build");
    return Ptr<Importer>();
}

Blob readTorchBlob(const String &filename, bool isBinary)
{
    CV_Error(Error::StsNotImplemented, "Torch importer is disabled in current build");
    return Blob();
}

#endif //defined(ENABLE_TORCH_IMPORTER) && ENABLE_TORCH_IMPORTER
}
}
