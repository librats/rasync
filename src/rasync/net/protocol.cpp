#include "net/protocol.h"

namespace rasync::proto {

const char* op_name(Op op) {
    switch (op) {
        case Op::Hello:          return "Hello";
        case Op::ManifestUpdate: return "ManifestUpdate";
        case Op::Request:      return "Request";
        case Op::FileStart:    return "FileStart";
        case Op::FileLiteral:  return "FileLiteral";
        case Op::FileCopy:     return "FileCopy";
        case Op::FileEnd:      return "FileEnd";
        case Op::FileAck:      return "FileAck";
        case Op::NotFound:     return "NotFound";
        case Op::RequestsDone: return "RequestsDone";
        case Op::Changed:      return "Changed";
        case Op::Bye:          return "Bye";
    }
    return "?";
}

} // namespace rasync::proto
