// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <system_error>
#include <variant>
#include <optional>
#include <string>

namespace uballoc {

enum class ErrorKind {
    OutOfBounds,
    Ioctl,
    Ftruncate,
    Mmap,
    Munmap,
    Mbind,
    Madvise,
    ShmOpen,
    ShmUnlink,
    Shm,
    ShmExists,
    Io,
};

class Error {
public:
    ErrorKind kind;
    std::string message;
    std::optional<std::error_code> code;
    
    explicit Error(ErrorKind k) : kind(k) {}
    Error(ErrorKind k, const std::string& msg) : kind(k), message(msg) {}
    Error(ErrorKind k, std::error_code ec) : kind(k), code(ec) {}
    Error(ErrorKind k, const std::string& msg, std::error_code ec) : kind(k), message(msg), code(ec) {}
    
    std::string to_string() const {
        switch (kind) {
            case ErrorKind::OutOfBounds: return "out-of-bounds memory access";
            case ErrorKind::Ioctl: return "ioctl error: " + (code ? code->message() : message);
            case ErrorKind::Ftruncate: return "ftruncate error: " + (code ? code->message() : message);
            case ErrorKind::Mmap: return "mmap error: " + (code ? code->message() : message);
            case ErrorKind::Munmap: return "munmap error: " + (code ? code->message() : message);
            case ErrorKind::Mbind: return "mbind error: " + (code ? code->message() : message);
            case ErrorKind::Madvise: return "madvise error: " + (code ? code->message() : message);
            case ErrorKind::ShmOpen: return "shm_open error: " + (code ? code->message() : message);
            case ErrorKind::ShmUnlink: return "shm_unlink error: " + (code ? code->message() : message);
            case ErrorKind::Shm: return "shm error: " + message;
            case ErrorKind::ShmExists: return "shm already exists";
            case ErrorKind::Io: return "io error: " + (code ? code->message() : message);
            default: return "unknown error";
        }
    }
};

template<typename T>
using Result = std::variant<T, Error>;

template<typename T>
inline bool is_ok(const Result<T>& r) { return std::holds_alternative<T>(r); }

template<typename T>
inline bool is_err(const Result<T>& r) { return std::holds_alternative<Error>(r); }

template<typename T>
inline T& unwrap(Result<T>& r) { return std::get<T>(r); }

template<typename T>
inline const T& unwrap(const Result<T>& r) { return std::get<T>(r); }

template<typename T>
inline Error& unwrap_err(Result<T>& r) { return std::get<Error>(r); }

}