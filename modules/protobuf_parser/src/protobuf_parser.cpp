// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2017, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective owners.

#include <map>
#include <string>
#include <vector>
#include <fstream>
#include <utility>
#include <algorithm>

#include "proto_descriptors.hpp"
#include "proto_terms.hpp"

namespace cv { namespace pb {

// Remove comments from prototxt file. Let comment is a sequence of characters
// that starts from '#' (inclusive) and ends by '\n' (inclusive).
static std::string removeProtoComments(const std::string& str)
{
    std::string res = "";
    bool isComment = false;
    for (size_t i = 0, n = str.size(); i < n; ++i)
    {
        if (str[i] == '#')
        {
            isComment = true;
        }
        else
        {
            if (isComment)
            {
                isComment = str[i] != '\n';
            }
            else
            {
                res += str[i];
            }
        }
    }
    return res;
}

// Split source text by tokens.
// Delimeters are specific for protobuf in text format.
static std::vector<std::string> tokenize(const std::string& str)
{
    std::vector<std::string> tokens;
    tokens.reserve(max(1, (int)str.size() / 7));

    std::string token = "";
    for (size_t i = 0, n = str.size(); i < n; ++i)
    {
        char symbol = str[i];
        if (symbol == ' ' || symbol == '\t' || symbol == '\r' ||
            symbol == '\n' || symbol == ':' || symbol == '\"' || symbol == ';')
        {
            if (!token.empty())
            {
                tokens.push_back(token);
                token = "";
            }
        }
        else if (symbol == '{' || symbol == '}')
        {
            if (!token.empty())
            {
                tokens.push_back(token);
                token = "";
            }
            tokens.push_back(std::string(1, symbol));
        }
        else
        {
            token += symbol;
        }
    }
    if (!token.empty())
    {
        tokens.push_back(token);
    }
    return tokens;
}

// Recursive function for the next procedure.
static void extractTypeNodes(const ProtobufNode& types,
                             const std::string& parentTypeName,
                             std::map<std::string, ProtobufNode>& typeNodes)
{
    std::string typeName;
    ProtobufNode typeNode;
    for (int i = 0, n = types.size(); i < n; ++i)
    {
        typeNode = types[i];

        CV_Assert(typeNode.has("name"));
        typeNode["name"] >> typeName;
        typeName = parentTypeName + "." + typeName;

        std::pair<std::string, ProtobufNode> mapValue(typeName, typeNode);
        CV_Assert(typeNodes.insert(mapValue).second);

        if (typeNode.has("message_type"))
            extractTypeNodes(typeNode["message_type"], typeName, typeNodes);
        if (typeNode.has("enum_type"))
            extractTypeNodes(typeNode["enum_type"], typeName, typeNodes);
    }
}

// Extract all nodes for combined types -- messages and enums.
// Map them by names.
static void extractTypeNodes(const ProtobufNode& protoRoot,
                             std::map<std::string, ProtobufNode>& typeNodes)
{
    std::string packageName = "";
    if (protoRoot.has("package"))
    {
        protoRoot["package"] >> packageName;
        packageName = "." + packageName;
    }

    if (protoRoot.has("message_type"))
        extractTypeNodes(protoRoot["message_type"], packageName, typeNodes);
    if (protoRoot.has("enum_type"))
        extractTypeNodes(protoRoot["enum_type"], packageName, typeNodes);
}

static Ptr<ProtobufField> buildEnum(const std::string& name,
                                    const std::map<std::string, ProtobufNode>& typeNodes,
                                    const std::string& defaultValue, bool packed)
{
    if (typeNodes.find(name) == typeNodes.end())
        CV_Error(Error::StsParseError, "Enum " + name + " not found");
    const ProtobufNode& enumNode = typeNodes.find(name)->second;

    Ptr<ProtoEnum> enumValue(new ProtoEnum(packed));
    ProtobufNode values = enumNode["value"];
    for (int i = 0; i < values.size(); ++i)
    {
        enumValue->addValue(values[i]["name"], values[i]["number"]);
    }
    enumValue->value = defaultValue;
    return enumValue;
}

static Ptr<ProtobufMessage> buildMessage(const std::string& name,
                                         const std::map<std::string, ProtobufNode>& typeNodes,
                                         std::map<std::string, Ptr<ProtobufMessage> >& builtMessages,
                                         bool proto3)
{
    // Try to find already built message.
    if (builtMessages.find(name) != builtMessages.end())
        return builtMessages[name]->clone().dynamicCast<ProtobufMessage>();

    if (typeNodes.find(name) == typeNodes.end())
        CV_Error(Error::StsParseError, "Message name " + name + " not found");
    const ProtobufNode& messageNode = typeNodes.find(name)->second;

    Ptr<ProtobufMessage> message(new ProtobufMessage());
    builtMessages[name] = message;

    // Get fields.
    ProtobufNode fields = messageNode["field"];
    for (int i = 0; i < fields.size(); ++i)
    {
        ProtobufNode fieldNode = fields[i];

        CV_Assert(fieldNode.has("name"));
        CV_Assert(fieldNode.has("number"));
        CV_Assert(fieldNode.has("type"));
        CV_Assert(fieldNode.has("label"));

        std::string fieldName = fieldNode["name"], fieldType;
        int fieldTag = fieldNode["number"], fieldTypeId = fieldNode["type"];

        // Field type.
        fieldType = fieldNode.has("type_name") ? fieldNode["type_name"] :
                                                 typeNameById(fieldTypeId);

        // Default value.
        std::string defaultValue = "";
        if (fieldNode.has("default_value"))
        {
            fieldNode["default_value"] >> defaultValue;
        }

        bool packed = (fieldNode.has("options") &&
                      fieldNode["options"].has("packed") &&
                      fieldNode["options"]["packed"]) ||
                      (proto3 && labelById(fieldNode["label"]) == "repeated");

        Ptr<ProtobufField> field;
        if (typeNameById(fieldTypeId) == "message")
        {
            field = buildMessage(fieldType, typeNodes, builtMessages, proto3);
        }
        else if (typeNameById(fieldTypeId) == "enum")
        {
            field = buildEnum(fieldType, typeNodes, defaultValue, packed);
        }
        else  // One of the simple types: int32, float, string, etc.
        {
            field = createField(fieldType, defaultValue, packed);
        }
        if (field.empty())
            CV_Error(Error::StsParseError, "Type name " + name + " not found");
        message->addField(field, fieldName, fieldTag, !defaultValue.empty());
    }
    return message;
};

ProtobufParser::ProtobufParser(const std::string& filePath, const std::string& msg)
{
    std::ifstream ifs(filePath.c_str(), std::ios::binary);
    CV_Assert(ifs.is_open());
    init(ifs, msg);
}

ProtobufParser::ProtobufParser(char* bytes, int numBytes, const std::string& msg)
{
    std::istringstream s(std::string(bytes, numBytes));
    init(s, msg);
}

ProtobufParser::ProtobufParser(std::istream& s, const std::string& msg)
{
    init(s, msg);
}

void ProtobufParser::init(std::istream& s, const std::string& msg)
{
    FileDescriptorSet protoDescriptor;
    protoDescriptor.read(s);

    std::map<std::string, ProtobufNode> typeNodes;
    std::map<std::string, Ptr<ProtobufMessage> > builtMessages;
    bool proto3 = false;
    for (int i = 0, n = protoDescriptor["file"].size(); i < n; ++i)
    {
        extractTypeNodes(protoDescriptor["file"][i], typeNodes);
        proto3 = proto3 || (protoDescriptor["file"][i].has("syntax") &&
                 (std::string)protoDescriptor["file"][i]["syntax"] == "proto3");
    }
    message = buildMessage(msg, typeNodes, builtMessages, proto3);
}

void ProtobufParser::parse(const std::string& filePath, bool text)
{
    if (text)
    {
        std::ifstream ifs(filePath.c_str());
        CV_Assert(ifs.is_open());

        ifs.seekg(0, std::ios::end);
        std::string content((int)ifs.tellg(), ' ');
        ifs.seekg(0, std::ios::beg);
        ifs.read(&content[0], content.size());
        ifs.close();
        // Add brackets to unify top-level message format. It's easier for text
        // format because in binary format we must write Varint value with
        // top message length.
        content = '{' + content + '}';

        content = removeProtoComments(content);
        std::vector<std::string> tokens = tokenize(content);
        std::vector<std::string>::iterator tokenIt = tokens.begin();
        message.dynamicCast<ProtobufMessage>()->read(tokenIt);
    }
    else
    {
        std::ifstream ifs(filePath.c_str(), std::ios::binary);
        CV_Assert(ifs.is_open());
        message.dynamicCast<ProtobufMessage>()->read(ifs);
    }
}

ProtobufNode ProtobufParser::operator[](const std::string& name) const
{
    return message.dynamicCast<ProtobufMessage>()->operator[](name);
}

bool ProtobufParser::has(const std::string& name) const
{
    return message.dynamicCast<ProtobufMessage>()->has(name);
}

void ProtobufParser::remove(const std::string& name, int idx)
{
    message.dynamicCast<ProtobufMessage>()->remove(name, idx);
}

ProtobufNode ProtobufParser::root() const
{
    return ProtobufNode(ProtobufFields(1, message));
}

}  // namespace pb
}  // namespace cv
