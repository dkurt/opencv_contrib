// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2017, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective owners.

#include "proto_message.hpp"

#include <map>
#include <vector>
#include <string>

#include "proto_terms.hpp"

namespace cv { namespace pb {

void ProtobufMessage::addField(const Ptr<ProtobufField>& field,
                               const std::string& name, int tag,
                               bool hasDefaultValue)
{
    fieldByTag[tag] = field;
    fieldByName[name] = field;
    nameByTag[tag] = name;
    readFields[name] = ProtobufFields();
    if (hasDefaultValue)
    {
        fieldsWithDefaultValue[name] = field;
    }
}

void ProtobufMessage::addField(const std::string& type, const std::string& name,
                               int tag)
{
    addField(createField(type), name, tag);
}

void ProtobufMessage::read(std::istream& s)
{
    // Drop all fields that were read before.
    readFields.clear();

    // Start parsing the message.
    int tag, wireType, msgEnd = INT_MAX;
    // Top level message has no length value at the beginning.
    bool isEmbedded = (int)s.tellg() != 0;
    if (isEmbedded)
    {
        // Embedded messages starts from length value.
        ProtoInt32 numBytes(s);
        msgEnd = (int)s.tellg() + numBytes.value;
    }

    std::map<int, Ptr<ProtobufField> >::iterator it;
    while (s.tellg() < msgEnd)
    {
        parseKey(s, &tag, &wireType);
        if (s.eof())
        {
            break;
        }

        it = fieldByTag.find(tag);
        if (it != fieldByTag.end())
        {
            // Parse bytes.
            Ptr<ProtobufField> copy = it->second->clone();
            copy->read(s);

            std::string name = nameByTag[tag];
            if (readFields.find(name) == readFields.end())
            {
                readFields[name] = ProtobufFields(1, copy);
            }
            else
            {
                readFields[name].push_back(copy);
            }
        }
        else
        {
            // Skip bytes.
            if (wireType == 0)            // Value with variable length.
                ProtoInt64 skipValue(s);  // Use value with maximal buffer.
            else if (wireType == 1)
                s.ignore(8);  // 64bit value.
            else if (wireType == 2)  // Some set of bytes with length value.
            {
                ProtoInt32 skip(s);
                s.ignore(skip.value);
            }
            else if (wireType == 5)
                s.ignore(4);  // 32bit value.
        }
    }
    CV_Assert(!isEmbedded || s.eof() || (int)s.tellg() == msgEnd);
}

void ProtobufMessage::read(std::vector<std::string>::iterator& tokenIt)
{
    // Drop all fields that were read before.
    readFields.clear();

    // Start parsing the message.
    CV_Assert(*tokenIt == "{");
    ++tokenIt;

    std::string fieldName;
    std::map<std::string, Ptr<ProtobufField> >::iterator it;
    while (*tokenIt != "}")
    {
        fieldName = *tokenIt;
        ++tokenIt;

        it = fieldByName.find(fieldName);
        if (it == fieldByName.end())
            CV_Error(Error::StsNotImplemented, "Skip in text format is not implemented");

        // Parse.
        Ptr<ProtobufField> copy = it->second->clone();
        copy->read(tokenIt);

        if (readFields.find(fieldName) == readFields.end())
        {
            readFields[fieldName] = ProtobufFields(1, copy);
        }
        else
        {
            readFields[fieldName].push_back(copy);
        }
    }
    CV_Assert(*tokenIt == "}");
    ++tokenIt;
}

Ptr<ProtobufField> ProtobufMessage::clone() const
{
    Ptr<ProtobufMessage> message(new ProtobufMessage());
    message->nameByTag = nameByTag;
    message->fieldByTag = fieldByTag;
    message->fieldByName = fieldByName;
    message->fieldsWithDefaultValue = fieldsWithDefaultValue;
    return message;
}

ProtobufNode ProtobufMessage::operator[](const std::string& name) const
{
    std::map<std::string, ProtobufFields>::const_iterator readFieldsIt;
    readFieldsIt = readFields.find(name);
    if (readFieldsIt != readFields.end())
    {
        return ProtobufNode(readFieldsIt->second);
    }

    std::map<std::string, Ptr<ProtobufField> >::const_iterator defaultFieldsIt;
    // Check that field has default value.
    defaultFieldsIt = fieldsWithDefaultValue.find(name);
    if (defaultFieldsIt != fieldsWithDefaultValue.end())
    {
        return ProtobufNode(ProtobufFields(1, defaultFieldsIt->second));
    }
    else
    {
        return ProtobufNode();
    }
}

bool ProtobufMessage::has(const std::string& name) const
{
    return readFields.find(name) != readFields.end();
}

void ProtobufMessage::remove(const std::string& name, int idx)
{
    std::map<std::string, ProtobufFields>::iterator it = readFields.find(name);
    CV_Assert(it != readFields.end());
    CV_Assert(0 <= idx && idx < (int)it->second.size());
    it->second.erase(it->second.begin() + idx);
}

}  // namespace pb
}  // namespace cv
