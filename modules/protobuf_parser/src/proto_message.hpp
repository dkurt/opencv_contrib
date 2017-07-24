// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2017, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective owners.

#ifndef __OPENCV_PROTOBUF_PARSER_PROTO_MESSAGE_HPP__
#define __OPENCV_PROTOBUF_PARSER_PROTO_MESSAGE_HPP__

#include <map>
#include <vector>
#include <string>

#include "precomp.hpp"

namespace cv { namespace pb {

// Structure that represents protobuf's message.
class ProtobufMessage : public ProtobufField
{
public:
    void addField(const Ptr<ProtobufField>& field, const std::string& name,
                  int tag, bool hasDefaultValue = false);

    void addField(const std::string& type, const std::string& name, int tag);

    virtual void read(std::istream& s);

    virtual void read(std::vector<std::string>::iterator& tokenIt);

    virtual Ptr<ProtobufField> clone() const;

    ProtobufNode operator[](const std::string& name) const;

    bool has(const std::string& name) const;

    void remove(const std::string& name, int idx = 0);

private:
    //! Map field names to data that was read.
    std::map<std::string, ProtobufFields> readFields;
    std::map<std::string, Ptr<ProtobufField> > fieldsWithDefaultValue;

    //! Use like parsing patterns. Copy to <readFields> during reading.
    std::map<int, Ptr<ProtobufField> > fieldByTag;
    std::map<std::string, Ptr<ProtobufField> > fieldByName;
    std::map<int, std::string> nameByTag;
};

}  // namespace pb
}  // namespace cv

#endif  // __OPENCV_PROTOBUF_PARSER_PROTO_MESSAGE_HPP__
