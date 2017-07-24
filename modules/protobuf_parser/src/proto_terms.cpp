// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2017, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective owners.

#include "proto_terms.hpp"

#include <map>
#include <vector>
#include <string>
#include <utility>
#include <algorithm>

namespace cv { namespace pb {

int readBinary(std::istream& s, void* dst, int numBytes)
{
    s.read((char*)dst, numBytes);
    CV_Assert(s.gcount() == numBytes || s.gcount() == 0);
    return (int)s.gcount();
}

void parseKey(std::istream& s, int* tag, int* wireType)
{
    ProtoInt32 keyValue(s);
    if (!s.eof())
    {
        int key = keyValue.value;
        *tag = (key >> 3);
        *wireType = key & 7;  // Last three bits.
        if (*tag <= 0)
        {
            CV_Error(Error::StsParseError, format("Unsupported tag value [%d]", *tag));
        }
        if (*wireType != 0 && *wireType != 1 && *wireType != 2 && *wireType != 5)
        {
            CV_Error(Error::StsParseError, format("Unsupported wire type [%d]", *wireType));
        }
    }
}

template<typename T>
static Ptr<ProtobufField> createProtoValue(const std::string& defaultValue,
                                           bool packed)
{
    return (packed ? ProtoPack::create<T>() :
                     Ptr<ProtobufField>(new ProtoValue<T>(defaultValue)));
}

Ptr<ProtobufField> createField(const std::string& type,
                               const std::string& defaultValue, bool packed)
{
    if (type == "int32")
        return createProtoValue<int32_t>(defaultValue, packed);
    else if (type == "uint32")
        return createProtoValue<uint32_t>(defaultValue, packed);
    else if (type == "int64")
        return createProtoValue<int64_t>(defaultValue, packed);
    else if (type == "uint64")
        return createProtoValue<uint64_t>(defaultValue, packed);
    else if (type == "float")
        return createProtoValue<float>(defaultValue, packed);
    else if (type == "double")
        return createProtoValue<double>(defaultValue, packed);
    else if (type == "bool")
        return createProtoValue<bool>(defaultValue, packed);
    else if (type == "string")
        return Ptr<ProtobufField>(new ProtoString(defaultValue));
    else
        CV_Error(Error::StsNotImplemented, "Unknown protobuf type " + type);
    return Ptr<ProtobufField>();
}

ProtoEnum::ProtoEnum(bool _packed) : packed(_packed) {}

void ProtoEnum::addValue(const std::string& name, int number)
{
    std::pair<int, std::string> enumValue(number, name);
    CV_Assert(enumValues.insert(enumValue).second);
}

void ProtoEnum::read(std::istream& s)
{
    int id = 0;
    if (packed)
    {
        Ptr<ProtoPack> pp = ProtoPack::create<int>().dynamicCast<ProtoPack>();
        pp->read(s);
        id = pp->operator[](pp->size() - 1).dynamicCast<ProtoInt32>()->value;
    }
    else
    {
        ProtoInt32 number(s);
        id = number.value;
    }
    std::map<int, std::string>::iterator it = enumValues.find(id);
    if (it == enumValues.end())
    {
        CV_Error(Error::StsParseError, format("Unknown enum value [%d]", id));
    }
    value = it->second;  // Set up string value.
}

Ptr<ProtobufField> ProtoEnum::clone() const
{
    Ptr<ProtoEnum> copy(new ProtoEnum(packed));
    copy->enumValues = enumValues;
    return copy.dynamicCast<ProtobufField>();
}

// For primitive types (varint, 32bit, 64bit) that has [packed = true] flag.
// See https://developers.google.com/protocol-buffers/docs/encoding#packed
template <typename T>
class ProtoPackImpl : public ProtoPack
{
public:
    virtual void read(std::istream& s)
    {
        values.clear();

        ProtoInt32 numBytes(s);
        if (typeid(T) == typeid(float) || typeid(T) == typeid(double) ||
            typeid(T) == typeid(bool))
        {
            CV_Assert(numBytes.value % sizeof(T) == 0);
            values.resize(numBytes.value / sizeof(T));

            const T& begin = values[0];
            CV_Assert(readBinary(s, (char*)&begin, numBytes.value));
        }
        else  // Type is varint.
        {
            values.reserve(min(1, numBytes.value / 4));

            int end = (int)s.tellg() + numBytes.value;
            while (s.tellg() < end)
            {
                singleValue.read(s);
                values.push_back(singleValue.value);
            }
            CV_Assert((int)s.tellg() == end);
        }
    }

    virtual Ptr<ProtobufField> clone() const
    {
        return Ptr<ProtobufField>(new ProtoPackImpl<T>());
    }

    virtual Ptr<ProtobufField> operator[](int idx) const
    {
        CV_Assert(0 <= idx && idx < (int)values.size());
        ProtoValue<T>* p = new ProtoValue<T>();
        p->value = values[idx];
        return Ptr<ProtobufField>(p);
    }

    virtual int size() const
    {
        return (int)values.size();
    }

    virtual void copyTo(int numBytes, void* dst) const
    {
        CV_Assert((size_t)numBytes == values.size() * sizeof(T));
        const T& begin = values[0];
        memcpy(dst, &begin, numBytes);
    }

    virtual void read(std::vector<std::string>::iterator& tokenIt)
    {
        // Just skip it.
        values.resize(1, valueFromString<T>(*tokenIt));
        ++tokenIt;
    }

private:
    // Use for stote template type.
    ProtoValue<T> singleValue;
    std::vector<T> values;
};

template <typename T>
Ptr<ProtobufField> ProtoPack::create()
{
    if (typeid(T) != typeid(int32_t) && typeid(T) != typeid(uint32_t) &&
        typeid(T) != typeid(int64_t) && typeid(T) != typeid(uint64_t) &&
        typeid(T) != typeid(float) && typeid(T) != typeid(double) &&
        typeid(T) != typeid(bool))
    {
        CV_Error(Error::StsParseError, "Unsupported packed field type");
    }
    return Ptr<ProtobufField>(new ProtoPackImpl<T>());
}

template <typename T>
T valueFromString(const std::string& str)
{
    T value = 0;
    if (!str.empty())
    {
        if (typeid(T) != typeid(bool))
        {
            std::stringstream ss(str);
            ss >> value;
        }
        else if (str == "true")
        {
            memset(&value, true, 1);
        }
        else if (str == "false")
        {
            memset(&value, false, 1);
        }
        else
        {
            CV_Error(Error::StsParseError,
                     "Cannot interpret boolean value: " + str);
        }
    }
    return value;
}

template <>
std::string valueFromString(const std::string& str) { return str; }

}  // namespace pb
}  // namespace cv
