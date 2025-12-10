#include "dht_krpc.h"
#include <random>
#include <sstream>
#include <stdexcept>

namespace torrent {
namespace dht {

// KRPCQuery implementation

bencode::BencodeValue KRPCQuery::toBencode() const {
    bencode::BencodeDict dict;

    // Transaction ID
    dict["t"] = bencode::BencodeValue(transaction_id);

    // Message type
    dict["y"] = bencode::BencodeValue(std::string("q"));

    // Query method
    dict["q"] = bencode::BencodeValue(getQueryName());

    // Arguments
    bencode::BencodeDict args;

    // Sender ID (required for all queries)
    std::string id_str(sender_id.begin(), sender_id.end());
    args["id"] = bencode::BencodeValue(id_str);

    // Query-specific arguments
    switch (query_type) {
        case QueryType::PING:
            // No additional arguments
            break;

        case QueryType::FIND_NODE:
            if (target.has_value()) {
                std::string target_str(target->begin(), target->end());
                args["target"] = bencode::BencodeValue(target_str);
            }
            break;

        case QueryType::GET_PEERS:
            if (info_hash.has_value()) {
                std::string hash_str(info_hash->begin(), info_hash->end());
                args["info_hash"] = bencode::BencodeValue(hash_str);
            }
            break;

        case QueryType::ANNOUNCE_PEER:
            if (info_hash.has_value()) {
                std::string hash_str(info_hash->begin(), info_hash->end());
                args["info_hash"] = bencode::BencodeValue(hash_str);
            }
            if (port.has_value()) {
                args["port"] = bencode::BencodeValue(static_cast<int64_t>(port.value()));
            }
            if (token.has_value()) {
                args["token"] = bencode::BencodeValue(token.value());
            }
            if (implied_port.has_value()) {
                args["implied_port"] = bencode::BencodeValue(static_cast<int64_t>(implied_port.value()));
            }
            break;
    }

    dict["a"] = bencode::BencodeValue(args);

    return bencode::BencodeValue(dict);
}

KRPCQuery KRPCQuery::fromBencode(const bencode::BencodeValue& value) {
    KRPCQuery query;

    if (!value.isDict()) {
        throw std::runtime_error("KRPC query must be a dictionary");
    }

    const auto& dict = value.asDict();

    // Parse transaction ID
    if (dict.find("t") != dict.end() && dict.at("t").isString()) {
        query.transaction_id = dict.at("t").asString();
    }

    // Parse query method
    if (dict.find("q") != dict.end() && dict.at("q").isString()) {
        std::string method = dict.at("q").asString();
        if (method == "ping") query.query_type = QueryType::PING;
        else if (method == "find_node") query.query_type = QueryType::FIND_NODE;
        else if (method == "get_peers") query.query_type = QueryType::GET_PEERS;
        else if (method == "announce_peer") query.query_type = QueryType::ANNOUNCE_PEER;
    }

    // Parse arguments
    if (dict.find("a") != dict.end() && dict.at("a").isDict()) {
        const auto& args = dict.at("a").asDict();

        // Sender ID
        if (args.find("id") != args.end() && args.at("id").isString()) {
            std::string id_str = args.at("id").asString();
            if (id_str.size() == NODE_ID_SIZE) {
                std::copy(id_str.begin(), id_str.end(), query.sender_id.begin());
            }
        }

        // Target (for find_node)
        if (args.find("target") != args.end() && args.at("target").isString()) {
            std::string target_str = args.at("target").asString();
            if (target_str.size() == NODE_ID_SIZE) {
                NodeID target_id;
                std::copy(target_str.begin(), target_str.end(), target_id.begin());
                query.target = target_id;
            }
        }

        // Info hash (for get_peers, announce_peer)
        if (args.find("info_hash") != args.end() && args.at("info_hash").isString()) {
            std::string hash_str = args.at("info_hash").asString();
            query.info_hash = std::vector<uint8_t>(hash_str.begin(), hash_str.end());
        }

        // Port (for announce_peer)
        if (args.find("port") != args.end() && args.at("port").isInt()) {
            query.port = static_cast<uint16_t>(args.at("port").asInt());
        }

        // Token (for announce_peer)
        if (args.find("token") != args.end() && args.at("token").isString()) {
            query.token = args.at("token").asString();
        }

        // Implied port (for announce_peer)
        if (args.find("implied_port") != args.end() && args.at("implied_port").isInt()) {
            query.implied_port = static_cast<int>(args.at("implied_port").asInt());
        }
    }

    return query;
}

std::string KRPCQuery::getQueryName() const {
    switch (query_type) {
        case QueryType::PING: return "ping";
        case QueryType::FIND_NODE: return "find_node";
        case QueryType::GET_PEERS: return "get_peers";
        case QueryType::ANNOUNCE_PEER: return "announce_peer";
    }
    return "unknown";
}

// KRPCResponse implementation

bencode::BencodeValue KRPCResponse::toBencode() const {
    bencode::BencodeDict dict;

    // Transaction ID
    dict["t"] = bencode::BencodeValue(transaction_id);

    // Message type
    dict["y"] = bencode::BencodeValue(std::string("r"));

    // Response values
    bencode::BencodeDict response;

    // Sender ID (required)
    std::string id_str(sender_id.begin(), sender_id.end());
    response["id"] = bencode::BencodeValue(id_str);

    // Nodes (for find_node, get_peers)
    if (nodes.has_value() && !nodes->empty()) {
        std::string compact = KRPCMessageFactory::encodeCompactNodes(nodes.value());
        response["nodes"] = bencode::BencodeValue(compact);
    }

    // Values (peer list for get_peers)
    if (values.has_value() && !values->empty()) {
        bencode::BencodeList vals;
        for (const auto& val : values.value()) {
            vals.push_back(bencode::BencodeValue(val));
        }
        response["values"] = bencode::BencodeValue(vals);
    }

    // Token (for get_peers)
    if (token.has_value()) {
        response["token"] = bencode::BencodeValue(token.value());
    }

    dict["r"] = bencode::BencodeValue(response);

    return bencode::BencodeValue(dict);
}

KRPCResponse KRPCResponse::fromBencode(const bencode::BencodeValue& value) {
    KRPCResponse response;

    if (!value.isDict()) {
        throw std::runtime_error("KRPC response must be a dictionary");
    }

    const auto& dict = value.asDict();

    // Parse transaction ID
    if (dict.find("t") != dict.end() && dict.at("t").isString()) {
        response.transaction_id = dict.at("t").asString();
    }

    // Parse response values
    if (dict.find("r") != dict.end() && dict.at("r").isDict()) {
        const auto& resp = dict.at("r").asDict();

        // Sender ID
        if (resp.find("id") != resp.end() && resp.at("id").isString()) {
            std::string id_str = resp.at("id").asString();
            if (id_str.size() == NODE_ID_SIZE) {
                std::copy(id_str.begin(), id_str.end(), response.sender_id.begin());
            }
        }

        // Nodes
        if (resp.find("nodes") != resp.end() && resp.at("nodes").isString()) {
            std::string compact = resp.at("nodes").asString();
            response.nodes = KRPCMessageFactory::parseCompactNodes(compact);
        }

        // Values (peer list)
        if (resp.find("values") != resp.end() && resp.at("values").isList()) {
            const auto& vals = resp.at("values").asList();
            std::vector<std::string> peer_values;
            for (const auto& val : vals) {
                if (val.isString()) {
                    peer_values.push_back(val.asString());
                }
            }
            response.values = peer_values;
        }

        // Token
        if (resp.find("token") != resp.end() && resp.at("token").isString()) {
            response.token = resp.at("token").asString();
        }
    }

    return response;
}

// KRPCError implementation

bencode::BencodeValue KRPCError::toBencode() const {
    bencode::BencodeDict dict;

    // Transaction ID
    dict["t"] = bencode::BencodeValue(transaction_id);

    // Message type
    dict["y"] = bencode::BencodeValue(std::string("e"));

    // Error value [code, message]
    bencode::BencodeList error_list;
    error_list.push_back(bencode::BencodeValue(static_cast<int64_t>(error_code)));
    error_list.push_back(bencode::BencodeValue(error_message));

    dict["e"] = bencode::BencodeValue(error_list);

    return bencode::BencodeValue(dict);
}

KRPCError KRPCError::fromBencode(const bencode::BencodeValue& value) {
    KRPCError error;

    if (!value.isDict()) {
        throw std::runtime_error("KRPC error must be a dictionary");
    }

    const auto& dict = value.asDict();

    // Parse transaction ID
    if (dict.find("t") != dict.end() && dict.at("t").isString()) {
        error.transaction_id = dict.at("t").asString();
    }

    // Parse error
    if (dict.find("e") != dict.end() && dict.at("e").isList()) {
        const auto& error_list = dict.at("e").asList();
        if (error_list.size() >= 2) {
            if (error_list[0].isInt()) {
                error.error_code = static_cast<ErrorCode>(error_list[0].asInt());
            }
            if (error_list[1].isString()) {
                error.error_message = error_list[1].asString();
            }
        }
    }

    return error;
}

// KRPCMessageFactory implementation

std::variant<KRPCQuery, KRPCResponse, KRPCError> KRPCMessageFactory::parse(
    const std::vector<uint8_t>& data) {

    bencode::BencodeValue value = bencode::decode(data);

    if (!value.isDict()) {
        throw std::runtime_error("Invalid KRPC message: not a dictionary");
    }

    const auto& dict = value.asDict();

    // Determine message type
    if (dict.find("y") == dict.end() || !dict.at("y").isString()) {
        throw std::runtime_error("Invalid KRPC message: missing type");
    }

    std::string type = dict.at("y").asString();

    if (type == "q") {
        return KRPCQuery::fromBencode(value);
    } else if (type == "r") {
        return KRPCResponse::fromBencode(value);
    } else if (type == "e") {
        return KRPCError::fromBencode(value);
    }

    throw std::runtime_error("Unknown KRPC message type: " + type);
}

KRPCQuery KRPCMessageFactory::createPing(const NodeID& sender_id, const std::string& tid) {
    KRPCQuery query;
    query.transaction_id = tid;
    query.query_type = QueryType::PING;
    query.sender_id = sender_id;
    return query;
}

KRPCQuery KRPCMessageFactory::createFindNode(const NodeID& sender_id,
                                             const NodeID& target,
                                             const std::string& tid) {
    KRPCQuery query;
    query.transaction_id = tid;
    query.query_type = QueryType::FIND_NODE;
    query.sender_id = sender_id;
    query.target = target;
    return query;
}

KRPCQuery KRPCMessageFactory::createGetPeers(const NodeID& sender_id,
                                             const std::vector<uint8_t>& info_hash,
                                             const std::string& tid) {
    KRPCQuery query;
    query.transaction_id = tid;
    query.query_type = QueryType::GET_PEERS;
    query.sender_id = sender_id;
    query.info_hash = info_hash;
    return query;
}

KRPCQuery KRPCMessageFactory::createAnnouncePeer(const NodeID& sender_id,
                                                 const std::vector<uint8_t>& info_hash,
                                                 uint16_t port,
                                                 const std::string& token,
                                                 const std::string& tid,
                                                 bool implied_port) {
    KRPCQuery query;
    query.transaction_id = tid;
    query.query_type = QueryType::ANNOUNCE_PEER;
    query.sender_id = sender_id;
    query.info_hash = info_hash;
    query.port = port;
    query.token = token;
    query.implied_port = implied_port ? 1 : 0;
    return query;
}

KRPCResponse KRPCMessageFactory::createPingResponse(const NodeID& sender_id,
                                                    const std::string& tid) {
    KRPCResponse response;
    response.transaction_id = tid;
    response.sender_id = sender_id;
    return response;
}

KRPCResponse KRPCMessageFactory::createFindNodeResponse(const NodeID& sender_id,
                                                        const std::vector<Node>& nodes,
                                                        const std::string& tid) {
    KRPCResponse response;
    response.transaction_id = tid;
    response.sender_id = sender_id;
    response.nodes = nodes;
    return response;
}

KRPCResponse KRPCMessageFactory::createGetPeersResponseNodes(const NodeID& sender_id,
                                                             const std::vector<Node>& nodes,
                                                             const std::string& token,
                                                             const std::string& tid) {
    KRPCResponse response;
    response.transaction_id = tid;
    response.sender_id = sender_id;
    response.nodes = nodes;
    response.token = token;
    return response;
}

KRPCResponse KRPCMessageFactory::createGetPeersResponseValues(const NodeID& sender_id,
                                                              const std::vector<std::string>& values,
                                                              const std::string& token,
                                                              const std::string& tid) {
    KRPCResponse response;
    response.transaction_id = tid;
    response.sender_id = sender_id;
    response.values = values;
    response.token = token;
    return response;
}

KRPCError KRPCMessageFactory::createError(ErrorCode code,
                                          const std::string& message,
                                          const std::string& tid) {
    KRPCError error(code, message);
    error.transaction_id = tid;
    return error;
}

std::vector<uint8_t> KRPCMessageFactory::serialize(const KRPCMessage& message) {
    bencode::BencodeValue value = message.toBencode();
    return bencode::encode(value);
}

std::string KRPCMessageFactory::generateTransactionId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 65535);

    uint16_t tid = dis(gen);
    std::string result(2, '\0');
    result[0] = (tid >> 8) & 0xFF;
    result[1] = tid & 0xFF;
    return result;
}

std::vector<Node> KRPCMessageFactory::parseCompactNodes(const std::string& compact) {
    std::vector<Node> nodes;

    // Each node is 26 bytes (20 bytes ID + 4 bytes IP + 2 bytes port)
    if (compact.size() % 26 != 0) {
        return nodes; // Invalid format
    }

    size_t count = compact.size() / 26;
    for (size_t i = 0; i < count; ++i) {
        std::vector<uint8_t> node_data(compact.begin() + i * 26,
                                       compact.begin() + (i + 1) * 26);
        try {
            nodes.push_back(Node::fromCompact(node_data));
        } catch (...) {
            // Skip invalid nodes
        }
    }

    return nodes;
}

std::string KRPCMessageFactory::encodeCompactNodes(const std::vector<Node>& nodes) {
    std::string result;
    result.reserve(nodes.size() * 26);

    for (const auto& node : nodes) {
        std::vector<uint8_t> compact = node.toCompact();
        result.append(compact.begin(), compact.end());
    }

    return result;
}

} // namespace dht
} // namespace torrent
