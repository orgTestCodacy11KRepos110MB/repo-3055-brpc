// Baidu RPC - A framework to host and access services throughout Baidu.
// Copyright (c) 2014 Baidu.com, Inc. All Rights Reserved

// Author: The baidu-rpc authors (pbrpc@baidu.com)
// Date: 2014/10/20 14:15:27

#include <gflags/gflags.h>
#include <stdio.h>                                      // getline
#include <string>                                       // std::string
#include <set>                                          // std::set
#include "bthread/bthread.h"                            // bthread_usleep
#include "base/iobuf.h"
#include "brpc/channel.h"
#include "brpc/policy/remote_file_naming_service.h"


namespace brpc {
namespace policy {

DEFINE_int32(remote_file_connect_timeout_ms, -1,
             "Timeout for creating connections to fetch remote server lists,"
             " set to remote_file_timeout_ms/3 by default (-1)");
DEFINE_int32(remote_file_timeout_ms, 1000,
             "Timeout for fetching remote server lists");

// Defined in file_naming_service.cpp
bool SplitIntoServerAndTag(const base::StringPiece& line,
                           base::StringPiece* server_addr,
                           base::StringPiece* tag);

static bool CutLineFromIOBuf(base::IOBuf* source, std::string* line_out) {
    if (source->empty()) {
        return false;
    }
    base::IOBuf line_data;
    if (source->cut_until(&line_data, "\n") != 0) {
        source->cutn(line_out, source->size());
        return true;
    }
    line_data.copy_to(line_out);
    if (!line_out->empty() && base::back_char(*line_out) == '\r') {
        line_out->resize(line_out->size() - 1);
    }
    return true;
}

int RemoteFileNamingService::GetServers(const char *service_name_cstr,
                                      std::vector<ServerNode>* servers) {
    servers->clear();

    if (_channel == NULL) {
        base::StringPiece tmpname(service_name_cstr);
        size_t pos = tmpname.find("://");
        base::StringPiece proto;
        if (pos != base::StringPiece::npos) {
            proto = tmpname.substr(0, pos);
            for (pos += 3; tmpname[pos] == '/'; ++pos) {}
            tmpname.remove_prefix(pos);
        } else {
            proto = "http";
        }
        if (proto != "bns" && proto != "http") {
            LOG(ERROR) << "Invalid protocol=`" << proto
                       << "\' in service_name=" << service_name_cstr;
            return -1;
        }
        size_t slash_pos = tmpname.find('/');
        base::StringPiece server_addr_piece;
        if (slash_pos == base::StringPiece::npos) {
            server_addr_piece = tmpname;
            _path = "/";
        } else {
            server_addr_piece = tmpname.substr(0, slash_pos);
            _path = tmpname.substr(slash_pos).as_string();
        }
        _server_addr.reserve(proto.size() + 3 + server_addr_piece.size());
        _server_addr.append(proto.data(), proto.size());
        _server_addr.append("://");
        _server_addr.append(server_addr_piece.data(), server_addr_piece.size());
        ChannelOptions opt;
        opt.protocol = PROTOCOL_HTTP;
        opt.connect_timeout_ms = FLAGS_remote_file_connect_timeout_ms > 0 ?
            FLAGS_remote_file_connect_timeout_ms : FLAGS_remote_file_timeout_ms / 3;
        opt.timeout_ms = FLAGS_remote_file_timeout_ms;
        std::unique_ptr<Channel> chan(new Channel);
        if (chan->Init(_server_addr.c_str(), "rr", &opt) != 0) {
            LOG(ERROR) << "Fail to init channel to " << _server_addr;
            return -1;
        }
        _channel.swap(chan);
    }

    Controller cntl;
    cntl.http_request().uri() = _path;
    _channel->CallMethod(NULL, &cntl, NULL, NULL, NULL);
    if (cntl.Failed()) {
        LOG(WARNING) << "Fail to access " << _server_addr << _path << ": "
                     << cntl.ErrorText();
        return -1;
    }
    std::string line;
    // Sort/unique the inserted vector is faster, but may have a different order
    // of addresses from the file. To make assertions in tests easier, we use
    // set to de-duplicate and keep the order.
    std::set<ServerNode> presence;

    while (CutLineFromIOBuf(&cntl.response_attachment(), &line)) {
        base::StringPiece addr;
        base::StringPiece tag;
        if (!SplitIntoServerAndTag(line, &addr, &tag)) {
            continue;
        }
        const_cast<char*>(addr.data())[addr.size()] = '\0'; // safe
        base::EndPoint point;
        if (str2endpoint(addr.data(), &point) != 0 &&
            hostname2endpoint(addr.data(), &point) != 0) {
            LOG(ERROR) << "Invalid address=`" << addr << '\'';
            continue;
        }
        ServerNode node;
        node.addr = point;
        tag.CopyToString(&node.tag);
        if (presence.insert(node).second) {
            servers->push_back(node);
        } else {
            RPC_VLOG << "Duplicated server=" << node;
        }
    }
    RPC_VLOG << "Got " << servers->size()
             << (servers->size() > 1 ? " servers" : " server")
             << " from " << service_name_cstr;
    return 0;
}

void RemoteFileNamingService::Describe(std::ostream& os,
                                     const DescribeOptions&) const {
    os << "remotefile";
    return;
}

NamingService* RemoteFileNamingService::New() const {
    return new RemoteFileNamingService;
}

void RemoteFileNamingService::Destroy() {
    delete this;
}

}  // namespace policy
} // namespace brpc
