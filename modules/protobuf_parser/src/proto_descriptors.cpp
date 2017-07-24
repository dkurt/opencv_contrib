// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2017, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective owners.

#include "proto_descriptors.hpp"

#include <string>

namespace cv { namespace pb {

    // Descriptor of field options.
    struct FieldOptionsDescriptor : public ProtobufMessage
    {
        FieldOptionsDescriptor()
        {
            addField("bool", "packed", 2);
        }

        static Ptr<ProtobufField> create()
        {
            return Ptr<ProtobufField>(new FieldOptionsDescriptor());
        }
    };

    // Descriptor of field definitions.
    struct FieldDescriptor : public ProtobufMessage
    {
        FieldDescriptor()
        {
            addField("string", "name", 1);
            addField("int32", "number", 3);
            addField("int32", "label", 4);  // optional, required, repeated.
            addField("int32", "type", 5);
            addField("string", "type_name", 6);
            addField("string", "default_value", 7);
            addField(FieldOptionsDescriptor::create(), "options", 8);
        }

        static Ptr<ProtobufField> create()
        {
            return Ptr<ProtobufField>(new FieldDescriptor());
        }
    };

    // Single enum value. Pair <name, number>.
    struct EnumValueDescriptor : public ProtobufMessage
    {
        EnumValueDescriptor()
        {
            addField("string", "name", 1);
            addField("int32", "number", 2);
        }

        static Ptr<ProtobufField> create()
        {
            return Ptr<ProtobufField>(new EnumValueDescriptor());
        }
    };

    // Descriptor of enum definitions.
    struct EnumDescriptor : public ProtobufMessage
    {
        EnumDescriptor()
        {
            addField("string", "name", 1);
            addField(EnumValueDescriptor::create(), "value", 2);
        }

        static Ptr<ProtobufField> create()
        {
            return Ptr<ProtobufField>(new EnumDescriptor());
        }
    };

    // Descriptor of message definitions.
    struct MessageDescriptor : public ProtobufMessage
    {
        explicit MessageDescriptor(int maxMsgDepth)
        {
            addField("string", "name", 1);
            addField(FieldDescriptor::create(), "field", 2);
            if (maxMsgDepth)
            {
                maxMsgDepth -= 1;
                // Use `message_type` instead `nested_type` for make it similar to FileDescriptor.
                addField(MessageDescriptor::create(maxMsgDepth), "message_type", 3);
            }
            addField(EnumDescriptor::create(), "enum_type", 4);
        }

        static Ptr<ProtobufField> create(int maxMsgDepth)
        {
            return Ptr<ProtobufField>(new MessageDescriptor(maxMsgDepth));
        }
    };

    // Definition of single `.proto` file.
    struct FileDescriptor : public ProtobufMessage
    {
        explicit FileDescriptor(int maxMsgDepth)
        {
            addField("string", "name", 1);
            addField("string", "package", 2);
            addField("string", "syntax", 12);
            addField(MessageDescriptor::create(maxMsgDepth), "message_type", 4);
            addField(EnumDescriptor::create(), "enum_type", 5);
        }

        static Ptr<ProtobufField> create(int maxMsgDepth)
        {
            return Ptr<ProtobufField>(new FileDescriptor(maxMsgDepth));
        }
    };

    FileDescriptorSet::FileDescriptorSet(int maxMsgDepth)
    {
        addField(FileDescriptor::create(maxMsgDepth), "file", 1);
    }

    std::string typeNameById(int id)
    {
        switch (id)
        {
            case 1: return "double";
            case 2: return "float";
            case 3: return "int64";
            case 4: return "uint64";
            case 5: return "int32";
            case 8: return "bool";
            case 9: case 12: return "string";
            case 11: return "message";
            case 13: return "uint32";
            case 14: return "enum";
            default:
            {
                CV_Error(Error::StsNotImplemented,
                         format("Unknown protobuf type id [%d]", id));
            }
        };
        return "";
    }

    std::string labelById(int id)
    {
        switch (id)
        {
            case 1: return "optional";
            case 2: return "required";
            case 3: return "repeated";
            default:
            {
                CV_Error(Error::StsNotImplemented,
                         format("Unknown protobuf label id [%d]", id));
            }
        }
        return "";
    }

}  // namespace pb
}  // namespace cv
