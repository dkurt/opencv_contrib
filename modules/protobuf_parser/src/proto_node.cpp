// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2017, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective owners.

#include "proto_terms.hpp"

#include <string>

#include "proto_message.hpp"

namespace cv { namespace pb {

ProtobufNode::ProtobufNode(const ProtobufFields& fields) : nodes(fields) {}

ProtobufNode ProtobufNode::operator[](const std::string& name) const
{
    CV_Assert(nodes.size() == 1);
    Ptr<ProtobufMessage> p = nodes[0].dynamicCast<ProtobufMessage>();
    CV_Assert(!p.empty());
    return p->operator[](name);
}

ProtobufNode ProtobufNode::operator[](const char* name) const
{
    return operator[](std::string(name));
}

ProtobufNode ProtobufNode::operator[](int idx) const
{
    if (nodes.size() == 1)
    {
        Ptr<ProtoPack> p = nodes[0].dynamicCast<ProtoPack>();
        if (!p.empty())
        {
            return ProtobufNode(ProtobufFields(1, p->operator[](idx)));
        }
    }
    CV_Assert(0 <= idx && idx < (int)nodes.size());
    return ProtobufNode(ProtobufFields(1, nodes[idx]));
}

bool ProtobufNode::empty() const { return nodes.empty(); }

int ProtobufNode::size() const
{
    if (nodes.size() == 1)
    {
        Ptr<ProtoPack> p = nodes[0].dynamicCast<ProtoPack>();
        return (p.empty() ? 1 : p->size());
    }
    return (int)nodes.size();
}

bool ProtobufNode::has(const std::string& name) const
{
    CV_Assert(nodes.size() == 1);
    Ptr<ProtobufMessage> p = nodes[0].dynamicCast<ProtobufMessage>();
    CV_Assert(!p.empty());
    return p->has(name);
}

template <typename T>
bool ProtobufNode::is() const
{
    if (nodes.size() != 1)
    {
        bool isType = true;
        for (size_t i = 0; isType && i < nodes.size(); ++i)
        {
            isType = ProtobufNode(ProtobufFields(1, nodes[i])).is<T>();
        }
        return isType;
    }

    Ptr<ProtobufField> field;
    Ptr<ProtoPack> pack = nodes[0].dynamicCast<ProtoPack>();
    if (pack.empty())
    {
        field = nodes[0];
    }
    else
    {
        CV_Assert(pack->size() >= 1);
        field = pack->operator[](0);
    }
    return !field.dynamicCast<ProtoValue<T> >().empty();
}

template <typename T>
T ProtobufNode::get() const
{
    CV_Assert(nodes.size() == 1);
    Ptr<ProtobufField> field;
    Ptr<ProtoPack> pack = nodes[0].dynamicCast<ProtoPack>();
    if (pack.empty())
    {
        field = nodes[0];
    }
    else
    {
        CV_Assert(pack->size() == 1);
        field = pack->operator[](0);
    }
    Ptr<ProtoValue<T> > ptr = field.dynamicCast<ProtoValue<T> >();
    if (ptr.empty()) CV_Error(Error::StsUnsupportedFormat, "Type mismatch");
    return ptr->value;
}

template <typename T>
void ProtobufNode::set(const T& value)
{
    CV_Assert(nodes.size() == 1);
    Ptr<ProtoValue<T> > ptr = nodes[0].dynamicCast<ProtoValue<T> >();
    if (ptr.empty()) CV_Error(Error::StsUnsupportedFormat, "Type mismatch");
    ptr->value = value;
}

void ProtobufNode::copyTo(int numBytes, void* dst) const
{
    if (nodes.size() == 1)
    {
        Ptr<ProtoPack> p = nodes[0].dynamicCast<ProtoPack>();
        if (!p.empty())
        {
            p->copyTo(numBytes, dst);
            return;
        }
    }
    if (isInt32())
      for (size_t i = 0; i < nodes.size(); ++i)
          ((int32_t*)dst)[i] = (int32_t)ProtobufNode(ProtobufFields(1, nodes[i]));
    else if (isUInt32())
        for (size_t i = 0; i < nodes.size(); ++i)
            ((uint32_t*)dst)[i] = (uint32_t)ProtobufNode(ProtobufFields(1, nodes[i]));
    else if (isInt64())
        for (size_t i = 0; i < nodes.size(); ++i)
            ((int64_t*)dst)[i] = (int64_t)ProtobufNode(ProtobufFields(1, nodes[i]));
    else if (isUInt64())
        for (size_t i = 0; i < nodes.size(); ++i)
            ((uint64_t*)dst)[i] = (uint64_t)ProtobufNode(ProtobufFields(1, nodes[i]));
    else if (isFloat())
        for (size_t i = 0; i < nodes.size(); ++i)
            ((float*)dst)[i] = (float)ProtobufNode(ProtobufFields(1, nodes[i]));
    else if (isDouble())
        for (size_t i = 0; i < nodes.size(); ++i)
            ((double*)dst)[i] = (double)ProtobufNode(ProtobufFields(1, nodes[i]));
    else if (isBool())
        for (size_t i = 0; i < nodes.size(); ++i)
            ((bool*)dst)[i] = (bool)ProtobufNode(ProtobufFields(1, nodes[i]));
    else
        CV_Error(Error::StsUnsupportedFormat, "Unknown data format");
}

void ProtobufNode::remove(const std::string& name, int idx)
{
    CV_Assert(nodes.size() == 1);
    Ptr<ProtobufMessage> p = nodes[0].dynamicCast<ProtobufMessage>();
    CV_Assert(!p.empty());
    return p->remove(name, idx);
}

void ProtobufNode::operator >> (int32_t& value) const { value = get<int32_t>(); }
void ProtobufNode::operator >> (uint32_t& value) const { value = get<uint32_t>(); }
void ProtobufNode::operator >> (int64_t& value) const { value = get<int64_t>(); }
void ProtobufNode::operator >> (uint64_t& value) const { value = get<uint64_t>(); }
void ProtobufNode::operator >> (float& value) const { value = get<float>(); }
void ProtobufNode::operator >> (double& value) const { value = get<double>(); }
void ProtobufNode::operator >> (bool& value) const { value = get<bool>(); }
void ProtobufNode::operator >> (std::string& str) const { str = get<std::string>(); }

ProtobufNode::operator int32_t() const { return get<int32_t>(); }
ProtobufNode::operator int64_t() const { return get<int64_t>(); }
ProtobufNode::operator uint32_t() const { return get<uint32_t>(); }
ProtobufNode::operator uint64_t() const { return get<uint64_t>(); }
ProtobufNode::operator float() const { return get<float>(); }
ProtobufNode::operator double() const { return get<double>(); }
ProtobufNode::operator bool() const { return get<bool>(); }
ProtobufNode::operator std::string() const { return get<std::string>(); }

void ProtobufNode::operator << (const std::string& str) { set<std::string>(str); }

bool ProtobufNode::isInt32() const { return is<int32_t>(); }
bool ProtobufNode::isUInt32() const { return is<uint32_t>(); }
bool ProtobufNode::isInt64() const { return is<int64_t>(); }
bool ProtobufNode::isUInt64() const { return is<uint64_t>(); }
bool ProtobufNode::isFloat() const { return is<float>(); }
bool ProtobufNode::isDouble() const { return is<double>(); }
bool ProtobufNode::isBool() const { return is<bool>(); }
bool ProtobufNode::isString() const { return is<std::string>(); }

}  // namespace pb
}  // namespace cv
