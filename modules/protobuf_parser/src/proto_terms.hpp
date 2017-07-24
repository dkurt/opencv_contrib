// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2017, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective owners.

#ifndef __OPENCV_PROTOBUF_PARSER_PROTO_TERMS_HPP__
#define __OPENCV_PROTOBUF_PARSER_PROTO_TERMS_HPP__

#include <map>
#include <vector>
#include <string>
#include <typeinfo>

#include "precomp.hpp"

namespace cv { namespace pb {

// Read <numBytes> bytes into <dst> from <s> binary stream.
// Returns number of bytes that were exactly read.
int readBinary(std::istream& s, void* dst, int numBytes);

// Read varint and extract tag and wire type. Wire type is one of the followings:
// | Wire type |                   Term types |
// |-----------|------------------------------|
// |         0 | int32, int64, uint32, uint64 |
// |           | sint32, sint64, bool, enum   |
// |         1 | fixed64, sfixed64, double    |
// |         2 | string, bytes,               |
// |           | embedded messages,           |
// |           | packed repeated fields       |
// |         5 | fixed32, sfixed32, float     |
// See https://developers.google.com/protocol-buffers/docs/encoding#structure
// Wire types 3 and 4 are deprecated.
void parseKey(std::istream& s, int* tag, int* wireType);

Ptr<ProtobufField> createField(const std::string& type,
                               const std::string& defaultValue = "",
                               bool packed = false);

template <typename T>
T valueFromString(const std::string& str);

// Explicit function for strings. It's used because of initialization of return
// value inside function. Only numeric types support initialization by zero int.
template <>
std::string valueFromString(const std::string& str);

template <typename T>
struct ProtoValue : public ProtobufField
{
    explicit ProtoValue(const std::string& defaultValue = "")
    {
        value = valueFromString<T>(defaultValue);
    }

    explicit ProtoValue(std::istream& s) { read(s); }

    virtual void read(std::istream& s)
    {
        if (typeid(T) == typeid(int32_t) || typeid(T) == typeid(uint32_t) ||
            typeid(T) == typeid(int64_t) || typeid(T) == typeid(uint64_t))
        {
            readVarint(s, &value, sizeof(T));
        }
        else if (typeid(T) == typeid(float) || typeid(T) == typeid(double) ||
                 typeid(T) == typeid(bool))
        {
            readBinary(s, &value, sizeof(T));
        }
        else if (typeid(T) == typeid(std::string))
        {
            ProtoValue<int32_t> len(s);
            if (len.value < 0)
                CV_Error(Error::StsParseError, "Negative string length");
            std::string* str = reinterpret_cast<std::string*>(&value);
            if (len.value != 0)
            {
                str->resize(len.value);
                CV_Assert(readBinary(s, &str->operator[](0), len.value));
            }
            else
                str->clear();
        }
        else
            CV_Error(Error::StsNotImplemented, "Unsupported protobuf value type");
    }

    virtual void read(std::vector<std::string>::iterator& tokenIt)
    {
        value = valueFromString<T>(*tokenIt);
        ++tokenIt;
    }

    virtual Ptr<ProtobufField> clone() const
    {
        return Ptr<ProtobufField>(new ProtoValue<T>());
    }

    T value;
private:
    static int readVarint(std::istream& s, void* dst, int maxNumBytes)
    {
        CV_Assert(0 <= maxNumBytes && maxNumBytes <= 8);
        uint64_t res = 0;
        char byte;
        bool read_next_byte = (readBinary(s, &byte, 1) != 0);
        int bytesRead = 0;
        // Read bytes until the first bit of byte is zero.
        // Maximal length - 9 bytes (7 bits from every byte , 63 bits totally).
        for (; bytesRead < 9 && read_next_byte; ++bytesRead)
        {
            read_next_byte = (byte & 0x80) != 0;
            uint64_t mask = (byte & 0x7f);
            res |= mask << bytesRead * 7;  // All bits except the last one.

            if (read_next_byte && !readBinary(s, &byte, 1))
            {
                CV_Error(Error::StsParseError, "Unexpected end of file");
            }
        }
        if (read_next_byte)
        {
            bytesRead += 1;
            read_next_byte = (byte & 0x80) != 0;
            CV_Assert(!read_next_byte);
        }
        memcpy(dst, &res, maxNumBytes);
        return bytesRead;
    }
};

typedef ProtoValue<int32_t> ProtoInt32;
typedef ProtoValue<uint32_t> ProtoUInt32;
typedef ProtoValue<int64_t> ProtoInt64;
typedef ProtoValue<uint64_t> ProtoUInt64;
typedef ProtoValue<float> ProtoFloat;
typedef ProtoValue<double> ProtoDouble;
typedef ProtoValue<bool> ProtoBool;
typedef ProtoValue<std::string> ProtoString;

class ProtoEnum : public ProtoString
{
public:
    ProtoEnum(bool packed);

    void addValue(const std::string& name, int number);

    virtual void read(std::istream& s);

    virtual Ptr<ProtobufField> clone() const;

private:
    bool packed;
    std::map<int, std::string> enumValues;
};

class ProtoPack : public ProtobufField
{
public:
    template <typename T>
    static Ptr<ProtobufField> create();

    virtual void read(std::istream& s) = 0;

    virtual Ptr<ProtobufField> clone() const = 0;

    virtual Ptr<ProtobufField> operator[](int idx) const = 0;

    virtual int size() const = 0;

    virtual void copyTo(int numBytes, void* dst) const = 0;

    virtual void read(std::vector<std::string>::iterator& tokenIt) = 0;
};

}  // namespace pb
}  // namespace cv

#endif  // __OPENCV_PROTOBUF_PARSER_PROTO_TERMS_HPP__
