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

#include "precomp.hpp"
#include <set>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iterator>
#include <opencv2/dnn/shape_utils.hpp>

using namespace cv;
using namespace cv::dnn;

using std::vector;
using std::map;
using std::make_pair;
using std::set;

namespace cv
{
namespace dnn
{

template<typename T>
static String toString(const T &v)
{
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

Mat blobFromImage(const Mat& image_, double scalefactor, bool swapRB)
{
    std::vector<Mat> images(1, image_);
    return blobFromImages(images, scalefactor, swapRB);
}

Mat blobFromImages(const std::vector<Mat>& images, double scalefactor, bool swapRB)
{
    size_t i, nimages = images.size();
    if(nimages == 0)
        return Mat();
    Mat image0 = images[0];
    int nch = image0.channels();
    CV_Assert(image0.dims == 2);
    Mat blob, image;
    if (nch == 3 || nch == 4)
    {
        int sz[] = { (int)nimages, 3, image0.rows, image0.cols };
        blob = Mat(4, sz, CV_32F);
        Mat ch[4];

        for( i = 0; i < nimages; i++ )
        {
            Mat image_ = images[i];
            if(image_.depth() == CV_8U)
            {
                image_.convertTo(image, CV_32F, scalefactor);
            }
            else
                image = image_;
            CV_Assert(image.depth() == CV_32F);
            nch = image.channels();
            CV_Assert(image.dims == 2 && (nch == 3 || nch == 4));
            CV_Assert(image.size() == image0.size());

            for( int j = 0; j < 3; j++ )
                ch[j] = Mat(image.rows, image.cols, CV_32F, blob.ptr((int)i, j));
            if(swapRB)
                std::swap(ch[0], ch[2]);
            split(image, ch);
        }
    }
    else
    {
       CV_Assert(nch == 1);
       int sz[] = { (int)nimages, 1, image0.rows, image0.cols };
       blob = Mat(4, sz, CV_32F);

       for( i = 0; i < nimages; i++ )
       {
           Mat image_ = images[i];
           if(image_.depth() == CV_8U)
           {
               image_.convertTo(image, CV_32F, scalefactor);
           }
           else
               image = image_;
           CV_Assert(image.depth() == CV_32F);
           nch = image.channels();
           CV_Assert(image.dims == 2 && (nch == 1));
           CV_Assert(image.size() == image0.size());

           image.copyTo(Mat(image.rows, image.cols, CV_32F, blob.ptr((int)i, 0)));
       }
    }
    return blob;
}


struct LayerPin
{
    int lid;
    int oid;

    LayerPin(int layerId = -1, int outputId = -1)
        : lid(layerId), oid(outputId) {}

    bool valid() const
    {
        return (lid >= 0 && oid >= 0);
    }

    bool equal(const LayerPin &r) const
    {
        return (lid == r.lid && oid == r.oid);
    }
};

struct LayerData
{
    LayerData() {}
    LayerData(int _id, const String &_name, const String &_type, LayerParams &_params)
        : id(_id), name(_name), type(_type), params(_params)
    {
        //add logging info
        params.name = name;
        params.type = type;
    }

    int id;
    String name;
    String type;
    LayerParams params;

    std::vector<LayerPin> inputBlobsId;
    std::set<int> inputLayersId;
    std::set<int> requiredOutputs;

    Ptr<Layer> layerInstance;
    std::vector<Mat> outputBlobs;
    std::vector<Mat*> inputBlobs;
    std::vector<Mat> internals;

    int flag;

    Ptr<Layer> getLayerInstance()
    {
        if (layerInstance)
            return layerInstance;

        layerInstance = LayerFactory::createLayerInstance(type, params);
        if (!layerInstance)
        {
            CV_Error(Error::StsError, "Can't create layer \"" + name + "\" of type \"" + type + "\"");
        }

        return layerInstance;
    }
};

//fake layer containing network input blobs
struct DataLayer : public Layer
{
    void finalize(const std::vector<Mat*>&, std::vector<Mat>&) {}
    void forward(std::vector<Mat*>&, std::vector<Mat>&, std::vector<Mat> &) {}

    int outputNameToIndex(String tgtName)
    {
        int idx = (int)(std::find(outNames.begin(), outNames.end(), tgtName) - outNames.begin());
        return (idx < (int)outNames.size()) ? idx : -1;
    }

    void setNames(const std::vector<String> &names)
    {
        outNames.assign(names.begin(), names.end());
    }

private:
    std::vector<String> outNames;
};

struct Net::Impl
{
    typedef std::vector<MatShape> ShapesVec;
    struct LayerShapes
    {
        ShapesVec in, out, internal;
        bool inplace;
        LayerShapes() {inplace = false;}
    };

    typedef std::map<int, LayerShapes> LayersShapesMap;
    typedef std::map<int, LayerData> MapIdToLayerData;

    Impl()
    {
        //allocate fake net input layer
        netInputLayer = Ptr<DataLayer>(new DataLayer());
        LayerData &inpl = layers.insert( make_pair(0, LayerData()) ).first->second;
        inpl.id = 0;
        inpl.name = "_input";
        inpl.type = "__NetInputLayer__";
        inpl.layerInstance = netInputLayer;
        layerNameToId.insert(std::make_pair(inpl.name, inpl.id));

        lastLayerId = 1;
        netWasAllocated = false;
    }

    Ptr<DataLayer> netInputLayer;
    std::vector<int> netOutputs;

    MapIdToLayerData layers;
    std::map<String, int> layerNameToId;

    int lastLayerId;

    bool netWasAllocated;

    void setUpNet()
    {
        if (!netWasAllocated)
        {
            allocateLayers();
            computeNetOutputLayers();

            netWasAllocated = true;
        }
    }

    int getLayerId(const String &layerName)
    {
        std::map<String, int>::iterator it = layerNameToId.find(layerName);
        return (it != layerNameToId.end()) ? it->second : -1;
    }

    int getLayerId(int id)
    {
        MapIdToLayerData::iterator it = layers.find(id);
        return (it != layers.end()) ? id : -1;
    }

    int getLayerId(DictValue &layerDesc)
    {
        if (layerDesc.isInt())
            return getLayerId(layerDesc.get<int>());
        else if (layerDesc.isString())
            return getLayerId(layerDesc.get<String>());

        CV_Assert(layerDesc.isInt() || layerDesc.isString());
        return -1;
    }

    String getLayerName(int id)
    {
        MapIdToLayerData::iterator it = layers.find(id);
        return (it != layers.end()) ? it->second.name : "(unknown layer)";
    }

    LayerData& getLayerData(int id)
    {
        MapIdToLayerData::iterator it = layers.find(id);

        if (it == layers.end())
            CV_Error(Error::StsObjectNotFound, format("Layer with requested id=%d not found", id));

        return it->second;
    }

    LayerData& getLayerData(const String &layerName)
    {
        int id = getLayerId(layerName);

        if (id < 0)
            CV_Error(Error::StsError, "Requsted layer \"" + layerName + "\" not found");

        return getLayerData(id);
    }

    LayerData& getLayerData(const DictValue &layerDesc)
    {
        if (layerDesc.isInt())
            return getLayerData(layerDesc.get<int>());
        else if (layerDesc.isString())
            return getLayerData(layerDesc.get<String>());

        CV_Assert(layerDesc.isInt() || layerDesc.isString());
        return *((LayerData*)NULL);
    }

    static void addLayerInput(LayerData &ld, int inNum, LayerPin from)
    {
        if ((int)ld.inputBlobsId.size() <= inNum)
        {
            ld.inputBlobsId.resize(inNum + 1);
        }
        else
        {
            LayerPin storedFrom = ld.inputBlobsId[inNum];
            if (storedFrom.valid() && !storedFrom.equal(from))
                CV_Error(Error::StsError, "Input #" + toString(inNum) + "of layer \"" + ld.name + "\" already was connected");
        }

        ld.inputBlobsId[inNum] = from;
    }

    static void splitPin(const String &pinAlias, String &layerName, String &outName)
    {
        size_t delimPos = pinAlias.find('.');
        layerName = pinAlias.substr(0, delimPos);
        outName = (delimPos == String::npos) ? String() : pinAlias.substr(delimPos + 1);
    }

    int resolvePinOutputName(LayerData &ld, const String &outName, bool isOutPin)
    {
        if (outName.empty())
            return 0;

        if (std::isdigit(outName[0]))
        {
            char *lastChar;
            long inum = std::strtol(outName.c_str(), &lastChar, 10);

            if (*lastChar == 0)
            {
                CV_Assert(inum == (int)inum);
                return (int)inum;
            }
        }

        if (isOutPin)
            return ld.getLayerInstance()->outputNameToIndex(outName);
        else
            return ld.getLayerInstance()->inputNameToIndex(outName);
    }

    LayerPin getPinByAlias(const String &pinAlias, bool isOutPin = true)
    {
        LayerPin pin;
        String layerName, outName;
        splitPin(pinAlias, layerName, outName);

        pin.lid = (layerName.empty()) ? 0 : getLayerId(layerName);

        if (pin.lid >= 0)
            pin.oid = resolvePinOutputName(getLayerData(pin.lid), outName, isOutPin);

        return pin;
    }

    void connect(int outLayerId, int outNum, int inLayerId, int inNum)
    {
        LayerData &ldOut = getLayerData(outLayerId);
        LayerData &ldInp = getLayerData(inLayerId);

        addLayerInput(ldInp, inNum, LayerPin(outLayerId, outNum));
        ldOut.requiredOutputs.insert(outNum);
    }

    void computeNetOutputLayers()
    {
        netOutputs.clear();

        MapIdToLayerData::iterator it;
        for (it = layers.begin(); it != layers.end(); it++)
        {
            int lid = it->first;
            LayerData &ld = it->second;

            if (ld.requiredOutputs.size() == 0)
                netOutputs.push_back(lid);
        }

        #ifndef NDEBUG
        std::cout << "\nNet Outputs(" << netOutputs.size() << "):\n";
        for (size_t i = 0; i < netOutputs.size(); i++)
            std::cout << layers[netOutputs[i]].name << "\n";
        #endif
    }

    #define CV_RETHROW_ERROR(err, newmsg)\
        cv::error(err.code, newmsg, err.func.c_str(), err.file.c_str(), err.line)

    void allocateLayer(int lid, const LayersShapesMap& layersShapes)
    {
        LayerData &ld = layers[lid];

        //already allocated
        if (ld.flag)
            return;

        size_t ninputs = ld.inputBlobsId.size();
#if 0
        printf("layer %s:", ld.name.c_str());
        for (size_t i = 0; i < ninputs; i++)
        {
            int inp_lid = ld.inputBlobsId[i].lid;
            LayerData &inp_ld = layers[inp_lid];
            int inp_outputs = (int)inp_ld.outputBlobs.size();
            std::cout << " " << inp_ld.name << "(" << inp_outputs;

            for( int j = 0; j < inp_outputs; j++ )
            {
                std::cout << (j == 0 ? ": " : ", ") << inp_ld.outputBlobs[j].size;
            }
            std::cout << ")";
        }
        printf("\n");
#endif

        //determine parent layers
        for (size_t i = 0; i < ninputs; i++)
            ld.inputLayersId.insert(ld.inputBlobsId[i].lid);

        //allocate parents
        for (set<int>::iterator i = ld.inputLayersId.begin(); i != ld.inputLayersId.end(); i++)
            allocateLayer(*i, layersShapes);

        //bind inputs
        ld.inputBlobs.resize(ninputs);
        for (size_t i = 0; i < ninputs; i++)
        {
            LayerPin from = ld.inputBlobsId[i];
            CV_Assert(from.valid());
            CV_DbgAssert(layers.count(from.lid) && (int)layers[from.lid].outputBlobs.size() > from.oid);
            ld.inputBlobs[i] = &layers[from.lid].outputBlobs[from.oid];
        }

        LayersShapesMap::const_iterator layerShapesIt = layersShapes.find(lid);

        CV_Assert(layerShapesIt != layersShapes.end());
        const ShapesVec& outShapes = layerShapesIt->second.out;
        CV_Assert(ld.requiredOutputs.size() <= outShapes.size());

        ld.outputBlobs.resize(std::max((size_t)1, outShapes.size())); //layer produce at least one output blob
        for(int i = 0; i < outShapes.size(); i++)
        {
            if (shape(ld.outputBlobs[i]) != outShapes[i])
            {
                if (layerShapesIt->second.inplace)
                {
                    CV_Assert(ld.inputBlobs.size() == ld.outputBlobs.size());
                    CV_Assert(ld.inputBlobs[i]->total() == total(outShapes[i]));
                    ld.outputBlobs[i] = ld.inputBlobs[i]->reshape(1, outShapes[i]);
                }
                else
                {
                    ld.outputBlobs[i].create(outShapes[i], CV_32F);
                }
            }
        }

        const ShapesVec& intShapes = layerShapesIt->second.internal;
        ld.internals.resize(intShapes.size());
        for(int i = 0; i < intShapes.size(); i++)
        {
            if (shape(ld.internals[i]) != intShapes[i] && total(intShapes[i]))
                ld.internals[i].create(intShapes[i], CV_32F);
        }

        Ptr<Layer> layerPtr = ld.getLayerInstance();
        //try
        {
            layerPtr->finalize(ld.inputBlobs, ld.outputBlobs);
#if 0
            std::cout << "\toutputs:";
            size_t noutputs = ld.outputBlobs.size();
            for (size_t j = 0; j < noutputs; j++)
            {
                std::cout << (j == 0 ? " " : ", ") << ld.outputBlobs[j].size;
            }
            std::cout << "\n";
#endif
        }
        /*catch (const cv::Exception &err)
        {
            CV_RETHROW_ERROR(err, format("The following error occured while making allocate() for layer \"%s\": %s", ld.name.c_str(), err.err.c_str()));
        }*/

        ld.flag = 1;
    }

    void allocateLayers()
    {
        MapIdToLayerData::iterator it;
        for (it = layers.begin(); it != layers.end(); it++)
            it->second.flag = 0;

        CV_Assert(!layers[0].outputBlobs.empty());
        ShapesVec inputShapes;
        for(int i = 0; i < layers[0].outputBlobs.size(); i++)
        {
            CV_Assert(layers[0].outputBlobs[i].total());
            inputShapes.push_back(shape(layers[0].outputBlobs[i]));
        }
        LayersShapesMap layersShapes;
        getLayersShapes(inputShapes, layersShapes);

        for (it = layers.begin(); it != layers.end(); it++)
        {
            int lid = it->first;
            allocateLayer(lid, layersShapes);
        }
    }

    void forwardLayer(LayerData &ld, bool clearFlags = true)
    {
        if (clearFlags)
        {
            MapIdToLayerData::iterator it;
            for (it = layers.begin(); it != layers.end(); it++)
                it->second.flag = 0;
        }

        //already was forwarded
        if (ld.flag)
            return;

        //forward parents
        for (set<int>::iterator i = ld.inputLayersId.begin(); i != ld.inputLayersId.end(); i++)
        {
            forwardLayer(layers[*i], false);
        }

        //forward itself
        //try
        {
            ld.layerInstance->forward(ld.inputBlobs, ld.outputBlobs, ld.internals);
        }
        /*catch (const cv::Exception &err)
        {
            CV_RETHROW_ERROR(err, format("The following error occured while making forward() for layer \"%s\": %s", ld.name.c_str(), err.err.c_str()));
        }*/

        ld.flag = 1;
    }

    void forwardAll()
    {
        MapIdToLayerData::iterator it;
        for (it = layers.begin(); it != layers.end(); it++)
            it->second.flag = 0;

        for (it = layers.begin(); it != layers.end(); it++)
            forwardLayer(it->second, false);
    }

    void getLayerShapesRecursively(int id, LayersShapesMap& inOutShapes)
    {
        std::vector<LayerPin>& inputLayerIds = layers[id].inputBlobsId;

        if (inOutShapes[id].in.empty())
        {
            for(int i = 0; i < inputLayerIds.size(); i++)
            {
                int layerId = inputLayerIds[i].lid;
                LayersShapesMap::iterator it =
                        inOutShapes.find(layerId);
                if(it == inOutShapes.end() ||
                        it->second.out.empty())
                {
                    getLayerShapesRecursively(layerId, inOutShapes);
                }
                const MatShape& shape = inOutShapes[layerId].out[inputLayerIds[i].oid];
                inOutShapes[id].in.push_back(shape);
            }
        }
        const ShapesVec& is = inOutShapes[id].in;
        ShapesVec& os = inOutShapes[id].out;
        ShapesVec& ints = inOutShapes[id].internal;
        int requiredOutputs = layers[id].requiredOutputs.size();
        inOutShapes[id].inplace =
                layers[id].getLayerInstance()->getMemoryShapes(is, requiredOutputs, os, ints);
    }

    void getLayersShapes(const ShapesVec& netInputShapes,
                         LayersShapesMap& inOutShapes)
    {
        inOutShapes.clear();

        inOutShapes[0].in = netInputShapes; //insert shape for first input layer
        for (MapIdToLayerData::iterator it = layers.begin();
             it != layers.end(); it++)
        {
            getLayerShapesRecursively(it->first, inOutShapes);
        }
    }

    void getLayerShapes(const ShapesVec& netInputShapes,
                        const int layerId,
                        LayerShapes& shapes)
    {
        LayersShapesMap inOutShapes;
        inOutShapes[0].in = netInputShapes; //insert shape for first input layer
        getLayerShapesRecursively(layerId, inOutShapes);
        shapes = inOutShapes[layerId];
    }
};

Net::Net() : impl(new Net::Impl)
{
}

Net::~Net()
{
}

int Net::addLayer(const String &name, const String &type, LayerParams &params)
{
    if (name.find('.') != String::npos)
    {
        CV_Error(Error::StsBadArg, "Added layer name \"" + name + "\" must not contain dot symbol");
        return -1;
    }

    if (impl->getLayerId(name) >= 0)
    {
        CV_Error(Error::StsBadArg, "Layer \"" + name + "\" already into net");
        return -1;
    }

    int id = ++impl->lastLayerId;
    impl->layerNameToId.insert(std::make_pair(name, id));
    impl->layers.insert(std::make_pair(id, LayerData(id, name, type, params)));

    return id;
}

int Net::addLayerToPrev(const String &name, const String &type, LayerParams &params)
{
    int prvLid = impl->lastLayerId;
    int newLid = this->addLayer(name, type, params);
    this->connect(prvLid, 0, newLid, 0);
    return newLid;
}

void Net::connect(int outLayerId, int outNum, int inpLayerId, int inpNum)
{
    impl->connect(outLayerId, outNum, inpLayerId, inpNum);
}

void Net::connect(String _outPin, String _inPin)
{
    LayerPin outPin = impl->getPinByAlias(_outPin);
    LayerPin inpPin = impl->getPinByAlias(_inPin);

    CV_Assert(outPin.valid() && inpPin.valid());

    impl->connect(outPin.lid, outPin.oid, inpPin.lid, inpPin.oid);
}

void Net::allocate()
{
    impl->setUpNet();
}

void Net::forward(LayerId toLayer)
{
    impl->setUpNet();

    if (toLayer.isString() && toLayer.get<String>().empty())
        impl->forwardAll();
    else
        impl->forwardLayer(impl->getLayerData(toLayer));
}

void Net::setNetInputs(const std::vector<String> &inputBlobNames)
{
    impl->netInputLayer->setNames(inputBlobNames);
}

void Net::setBlob(String outputName, const Mat &blob_)
{
    LayerPin pin = impl->getPinByAlias(outputName);
    if (!pin.valid())
        CV_Error(Error::StsObjectNotFound, "Requested blob \"" + outputName + "\" not found");

    LayerData &ld = impl->layers[pin.lid];
    ld.outputBlobs.resize( std::max(pin.oid+1, (int)ld.requiredOutputs.size()) );
    MatShape prevShape = shape(ld.outputBlobs[pin.oid]);
    ld.outputBlobs[pin.oid] = blob_.clone();

    impl->netWasAllocated = impl->netWasAllocated && prevShape == shape(blob_);
}

Mat Net::getBlob(String outputName)
{
    LayerPin pin = impl->getPinByAlias(outputName);
    if (!pin.valid())
        CV_Error(Error::StsObjectNotFound, "Requested blob \"" + outputName + "\" not found");

    LayerData &ld = impl->layers[pin.lid];
    if ((size_t)pin.oid >= ld.outputBlobs.size())
    {
        CV_Error(Error::StsOutOfRange, "Layer \"" + ld.name + "\" produce only " + toString(ld.outputBlobs.size()) +
                                       " outputs, the #" + toString(pin.oid) + " was requsted");
    }
    return ld.outputBlobs[pin.oid];
}

Mat Net::getParam(LayerId layer, int numParam)
{
    LayerData &ld = impl->getLayerData(layer);

    std::vector<Mat> &layerBlobs = ld.layerInstance->blobs;
    CV_Assert(numParam < (int)layerBlobs.size());
    return layerBlobs[numParam];
}

void Net::setParam(LayerId layer, int numParam, const Mat &blob)
{
    LayerData &ld = impl->getLayerData(layer);

    std::vector<Mat> &layerBlobs = ld.layerInstance->blobs;
    CV_Assert(numParam < (int)layerBlobs.size());
    //we don't make strong checks, use this function carefully
    layerBlobs[numParam] = blob;
}

int Net::getLayerId(const String &layer)
{
    return impl->getLayerId(layer);
}

void Net::deleteLayer(LayerId)
{
    CV_Error(Error::StsNotImplemented, "");
}

Ptr<Layer> Net::getLayer(LayerId layerId)
{
    LayerData &ld = impl->getLayerData(layerId);
    if (!ld.layerInstance)
        CV_Error(Error::StsNullPtr, format("Requested layer \"%s\" was not initialized", ld.name.c_str()));
    return ld.layerInstance;
}

std::vector<Ptr<Layer> > Net::getLayerInputs(LayerId layerId)
{
    LayerData &ld = impl->getLayerData(layerId);
    if (!ld.layerInstance)
        CV_Error(Error::StsNullPtr, format("Requested layer \"%s\" was not initialized", ld.name.c_str()));

    std::vector<Ptr<Layer> > inputLayers;
    inputLayers.reserve(ld.inputLayersId.size());
    std::set<int>::iterator it;
    for (it = ld.inputLayersId.begin(); it != ld.inputLayersId.end(); ++it) {
        inputLayers.push_back(getLayer(*it));
    }
    return inputLayers;
}

std::vector<String> Net::getLayerNames() const
{
    std::vector<String> res;
    res.reserve(impl->layers.size());

    Impl::MapIdToLayerData::iterator it;
    for (it = impl->layers.begin(); it != impl->layers.end(); it++)
    {
        if (it->second.id) //skip Data layer
            res.push_back(it->second.name);
    }

    return res;
}

bool Net::empty() const
{
    return impl->layers.size() <= 1; //first layer is default Data layer
}

std::vector<int> Net::getUnconnectedOutLayers() const
{
    std::vector<int> layersIds;

    Impl::MapIdToLayerData::iterator it;
    for (it = impl->layers.begin(); it != impl->layers.end(); it++)
    {
        int lid = it->first;
        LayerData &ld = it->second;

        if (ld.requiredOutputs.size() == 0)
            layersIds.push_back(lid);
    }

    return layersIds;
}

void Net::getLayersShapes(const Net::Impl::ShapesVec& netInputShapes,
                          std::vector<int>* layersIds,
                          std::vector<Net::Impl::ShapesVec>* inLayersShapes,
                          std::vector<Net::Impl::ShapesVec>* outLayersShapes) const
{
    if ((layersIds || inLayersShapes || outLayersShapes) == false)
        return;

    if (layersIds) layersIds->clear();
    if (inLayersShapes) inLayersShapes->clear();
    if (outLayersShapes) outLayersShapes->clear();

    Impl::LayersShapesMap inOutShapes;
    impl->getLayersShapes(netInputShapes, inOutShapes);

    for(Impl::LayersShapesMap::const_iterator it = inOutShapes.begin();
        it != inOutShapes.end(); it++)
    {
        if (layersIds)
            layersIds->push_back(it->first);
        if (inLayersShapes)
            inLayersShapes->push_back(it->second.in);
        if (outLayersShapes)
            outLayersShapes->push_back(it->second.out);
    }
}

void Net::getLayersShapes(const MatShape& netInputShape,
                          std::vector<int>* layerIds,
                          std::vector<Net::Impl::ShapesVec>* inLayersShapes,
                          std::vector<Net::Impl::ShapesVec>* outLayersShapes) const
{
    getLayersShapes(Net::Impl::ShapesVec(1, netInputShape),
                    layerIds, inLayersShapes, outLayersShapes);
}

void Net::getLayerShapes(const MatShape& netInputShape,
                         const int layerId,
                         Net::Impl::ShapesVec* inLayerShapes,
                         Net::Impl::ShapesVec* outLayerShapes) const
{
    getLayerShapes(Net::Impl::ShapesVec(1, netInputShape),
                   layerId, inLayerShapes, outLayerShapes);

}

void Net::getLayerShapes(const Net::Impl::ShapesVec& netInputShapes,
                    const int layerId,
                    Net::Impl::ShapesVec* inLayerShapes,
                    Net::Impl::ShapesVec* outLayerShapes) const
{
    Impl::LayerShapes shapes;
    impl->getLayerShapes(netInputShapes, layerId, shapes);
    if (inLayerShapes)
        *inLayerShapes = shapes.in;
    if (outLayerShapes)
        *outLayerShapes = shapes.out;
}

int64 Net::getFLOPS(const std::vector<MatShape>& netInputShapes) const
{
    int64 flops = 0;
    std::vector<int> ids;
    std::vector<std::vector<MatShape> > inShapes, outShapes;
    getLayersShapes(netInputShapes, &ids, &inShapes, &outShapes);
    CV_Assert(inShapes.size() == outShapes.size());
    CV_Assert(inShapes.size() == ids.size());

    for(int i = 0; i < ids.size(); i++)
    {
        flops += impl->layers[ids[i]].getLayerInstance()->getFLOPS(inShapes[i],
                                                                   outShapes[i]);
    }

    return flops;
}

int64 Net::getFLOPS(const MatShape& netInputShape) const
{
    return getFLOPS(std::vector<MatShape>(1, netInputShape));
}

int64 Net::getFLOPS(const int layerId,
              const std::vector<MatShape>& netInputShapes) const
{
    Impl::MapIdToLayerData::iterator layer = impl->layers.find(layerId);
    CV_Assert(layer != impl->layers.end());

    Impl::LayerShapes shapes;
    impl->getLayerShapes(netInputShapes, layerId, shapes);

    return layer->second.getLayerInstance()->getFLOPS(shapes.in, shapes.out);
}

int64 Net::getFLOPS(const int layerId,
              const MatShape& netInputShape) const
{
    return getFLOPS(layerId, std::vector<MatShape>(1, netInputShape));
}

void Net::getLayerTypes(std::vector<String>& layersTypes) const
{
    layersTypes.clear();

    std::map<String, int> layers;
    for (Impl::MapIdToLayerData::iterator it = impl->layers.begin();
         it != impl->layers.end(); it++)
    {
        if (layers.find(it->second.type) == layers.end())
            layers[it->second.type] = 0;
        layers[it->second.type]++;
    }

    for (std::map<String, int>::iterator it = layers.begin();
         it != layers.end(); it++)
    {
        layersTypes.push_back(it->first);
    }
}

int Net::getLayersCount(const String& layerType) const
{
    int count = 0;
    for (Impl::MapIdToLayerData::iterator it = impl->layers.begin();
         it != impl->layers.end(); it++)
    {
        if (it->second.type == layerType)
            count++;
    }
    return count;
}

void Net::getMemoryConsumption(const int layerId,
                               const std::vector<MatShape>& netInputShapes,
                               size_t& weights, size_t& blobs) const
{
    Impl::MapIdToLayerData::iterator layer = impl->layers.find(layerId);
    CV_Assert(layer != impl->layers.end());

    weights = blobs = 0;

    for(int i = 0; i < layer->second.params.blobs.size(); i++)
    {
        const Mat& weightsBlob = layer->second.params.blobs[i];
        weights += weightsBlob.total()*weightsBlob.elemSize();
    }

    std::vector<MatShape> outLayerShapes;
    getLayerShapes(netInputShapes, layerId, 0, &outLayerShapes);
    for(int i = 0; i < outLayerShapes.size(); i++)
    {
        blobs += total(outLayerShapes[i]) * sizeof(float);
    }
}

void Net::getMemoryConsumption(const std::vector<MatShape>& netInputShapes,
                               size_t& weights, size_t& blobs) const
{
    std::vector<int> layerIds;
    std::vector<std::vector<MatShape> > outLayerShapes;

    getLayersShapes(netInputShapes, &layerIds, 0, &outLayerShapes);

    weights = blobs = 0;
    for(int i = 0; i < layerIds.size(); i++)
    {
        Impl::MapIdToLayerData::iterator layer = impl->layers.find(layerIds[i]);
        CV_Assert(layer != impl->layers.end());

        for(int j = 0; j < layer->second.params.blobs.size(); j++)
        {
            const Mat& weightsBlob = layer->second.params.blobs[j];
            weights += weightsBlob.total()*weightsBlob.elemSize();
        }

        for(int j = 0; j < outLayerShapes[i].size(); j++)
        {
            blobs += total(outLayerShapes[i][j]) * sizeof(float);
        }
    }
}

void Net::getMemoryConsumption(const int layerId,
                               const MatShape& netInputShape,
                               size_t& weights, size_t& blobs) const
{
    getMemoryConsumption(layerId, std::vector<MatShape>(1, netInputShape),
                         weights, blobs);
}

void Net::getMemoryConsumption(const MatShape& netInputShape,
                               size_t& weights, size_t& blobs) const
{
    getMemoryConsumption(std::vector<MatShape>(1, netInputShape),
                         weights, blobs);
}

//////////////////////////////////////////////////////////////////////////

Importer::~Importer() {}

Layer::Layer() {}

Layer::Layer(const LayerParams &params)
    : blobs(params.blobs), name(params.name), type(params.type)
{

}

void Layer::setParamsFrom(const LayerParams &params)
{
    blobs = params.blobs;
    name = params.name;
    type = params.type;
}

int Layer::inputNameToIndex(String)
{
    return -1;
}

int Layer::outputNameToIndex(String)
{
    return -1;
}

template <typename T>
static void vecToPVec(const std::vector<T> &v, std::vector<T*> &pv)
{
    pv.resize(v.size());
    for (size_t i = 0; i < v.size(); i++)
        pv[i] = const_cast<T*>(&v[i]);
}

void Layer::finalize(const std::vector<Mat> &inputs, std::vector<Mat> &outputs)
{
    std::vector<Mat*> inputsp;
    vecToPVec(inputs, inputsp);
    this->finalize(inputsp, outputs);
}

void Layer::finalize(const std::vector<Mat*> &input, std::vector<Mat> &output)
{
    (void)input;(void)output;
}

std::vector<Mat> Layer::finalize(const std::vector<Mat> &inputs)
{
    std::vector<Mat> outputs;
    this->finalize(inputs, outputs);
    return outputs;
}

void Layer::forward(const std::vector<Mat> &inputs, std::vector<Mat> &outputs, std::vector<Mat> &internals)
{
    std::vector<Mat*> inputsp;
    vecToPVec(inputs, inputsp);
    this->forward(inputsp, outputs, internals);
}

void Layer::run(const std::vector<Mat> &inputs, std::vector<Mat> &outputs, std::vector<Mat> &internals)
{
    std::vector<Mat*> inputsp;
    vecToPVec(inputs, inputsp);
    this->finalize(inputsp, outputs);
    this->forward(inputsp, outputs, internals);
}

Layer::~Layer() {}

bool Layer::getMemoryShapes(const std::vector<MatShape> &inputs,
                            const int requiredOutputs,
                            std::vector<MatShape> &outputs,
                            std::vector<MatShape> &internals) const
{
    CV_Assert(inputs.size());
    outputs.assign(std::max(requiredOutputs, (int)inputs.size()), inputs[0]);
    return false;
}

//////////////////////////////////////////////////////////////////////////

struct LayerFactory::Impl : public std::map<String, LayerFactory::Constuctor>
{
};

Ptr<LayerFactory::Impl> LayerFactory::impl ()
{
    // allocate on first use
    static Ptr<LayerFactory::Impl> impl_(new LayerFactory::Impl());
    return impl_;
}

void LayerFactory::registerLayer(const String &_type, Constuctor constructor)
{
    String type = _type.toLowerCase();
    Impl::iterator it = impl()->find(type);

    if (it != impl()->end() && it->second != constructor)
    {
        CV_Error(cv::Error::StsBadArg, "Layer \"" + type + "\" already was registered");
    }

    impl()->insert(std::make_pair(type, constructor));
}

void LayerFactory::unregisterLayer(const String &_type)
{
    String type = _type.toLowerCase();
    impl()->erase(type);
}

Ptr<Layer> LayerFactory::createLayerInstance(const String &_type, LayerParams& params)
{
    String type = _type.toLowerCase();
    Impl::const_iterator it = LayerFactory::impl()->find(type);

    if (it != impl()->end())
    {
        return it->second(params);
    }
    else
    {
        return Ptr<Layer>(); //NULL
    }
}

////////////////////////////////////////////////////////////////////////////////

// Replaces symbols to codes (i.e. whitespaces to %20, new lines to %0A).
static std::string toURL(const std::string& s)
{
    std::ostringstream ss;
    for (int i = 0, n = s.size(); i < n; ++i)
    {
        char symbol = s[i];
        if (' ' <= symbol && symbol <= '/' || ':' <= symbol && symbol <= '@' ||
            '[' <= symbol && symbol <= '`' || '{' <= symbol && symbol <= '~')
        {
            ss << "%" << std::hex << (int)symbol << std::dec;
        }
        else if (symbol == '\n')
        {
            ss << "%0A";
        }
        else
        {
            ss << symbol;
        }
    }
    return ss.str();
}

static std::string extractBuildInfo(const std::string& title)
{
    std::string buildInfo = getBuildInformation();

    int start = buildInfo.find(title);
    start = buildInfo.find_first_not_of(' ', start + title.size());
    int end = buildInfo.find('\n', start);
    // +1 for '\n' symbol
    return buildInfo.substr(start, end - start + 1);
}

void submitError(int code, std::string msg, const char* func, const char* file,
                 int line)
{
    std::string platform = extractBuildInfo("Host:");
    std::ostringstream body;
    body << "OpenCV: " << extractBuildInfo("Version control:");
    body << "OpenCV(contrib): " << extractBuildInfo("Version control (extra):");
    body << "Operating System: " << extractBuildInfo("Host:");
    body << "\nException at " << file << ":" << line;
    body << "\n" << msg;
    msg += ". To submit an issue follow link and add details: "
           "https://github.com/opencv/opencv_contrib/issues/new?&body=" + toURL(body.str());
    cv::error(code, msg, func, file, line);
}

}
}
