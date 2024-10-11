/// @license GNU LGPL
///
/// Distributed under the GNU LGPL License
/// (See accompanying file LICENSE or copy at
/// http://www.gnu.org/licenses/lgpl.html)
///

#pragma once

#include <opc/common/class_pointers.h>
#ifdef HAVE_SYSTEM_SPDLOG
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/fmt/ostr.h>
#else
#include <opc/spdlog/spdlog.h>
#include <opc/spdlog/fmt/ostr.h>
#endif

#define FUNCTION_LINE_NAME (std::string( __FUNCTION__ ) + std::string (":") + std::to_string(__LINE__))

#define __FUN(__s) (FUNCTION_LINE_NAME + "| " + __s).c_str()
//#define FLN			std::string (std::string( "(File: ") + std::string ( __FILE__ ) + std::string( ";Function: ") + std::string ( __FUNCTION__ ) + std::string ( ";Line: ") + std::to_string(__LINE__) + std::string (";)"))
//#define FLN2		std::string (std::string("\nFile: ") + std::string ( __FILE__ ) + std::string("\nFunction: ") + std::string ( __FUNCTION__ ) + std::string ("\nLine: ") + std::to_string(__LINE__) + std::string ("\n")) 

namespace Common
{

namespace Logger {
  DEFINE_CLASS_POINTERS(spdlog::logger)
};

}

#define LOG_TRACE(__logger__, ...) { if (__logger__ && __logger__->should_log(spdlog::level::trace)) { __logger__->trace(__VA_ARGS__); __logger__->flush();}}
#define LOG_DEBUG(__logger__, ...) { if (__logger__ && __logger__->should_log(spdlog::level::debug)) { __logger__->debug(__VA_ARGS__); __logger__->flush();}}
#define LOG_INFO(__logger__, ...) { if (__logger__ && __logger__->should_log(spdlog::level::info)) { __logger__->info(__VA_ARGS__); __logger__->flush();}}
#define LOG_WARN(__logger__, ...) { if (__logger__ && __logger__->should_log(spdlog::level::warn)) { __logger__->warn(__VA_ARGS__); __logger__->flush();}}
#define LOG_ERROR(__logger__, ...) { if (__logger__ && __logger__->should_log(spdlog::level::err)) { __logger__->error(__VA_ARGS__); __logger__->flush();}}
#define LOG_CRITICAL(__logger__, ...) { if (__logger__ && __logger__->should_log(spdlog::level::critical)) { __logger__->critical(__VA_ARGS__); __logger__->flush();}}


//#define LOGGER_TRACE(__logger__, ...) \
//{\
//    if(__logger__ && __logger__->should_log(spdlog::level::trace))\
//    {\
//        std::stringstream streams;\
//        streams << std::setw(90) << std::left << FUNCTION_LINE_NAME << std::string("{}");\
//        __logger__->trace(streams.str().c_str(), __VA_ARGS__);\
//        __logger__->flush();\
//    }\
//};
//
//#define LOGGER_DUBUF(__logger__, ...) \
//{\
//    if(__logger__ && __logger__->should_log(spdlog::level::debug))\
//    {\
//        std::stringstream streams;\
//        streams << std::setw(90) << std::left << FUNCTION_LINE_NAME << std::string("{}");\
//        __logger__->debug(streams.str().c_str(), __VA_ARGS__);\
//        __logger__->flush();\
//    }\
//};
//
//#define LOGGER_INFO(__logger__, ...) \
//{\
//    if(__logger__ && __logger__->should_log(spdlog::level::info))\
//    {\
//        std::stringstream streams;\
//        streams << std::setw(90) << std::left << FUNCTION_LINE_NAME << std::string("{}");\
//        __logger__->info(streams.str().c_str(), __VA_ARGS__);\
//        __logger__->flush();\
//    }\
//};
//
//#define LOGGER_WARN(__logger__, ...) \
//{\
//    if(__logger__ && __logger__->should_log(spdlog::level::warn))\
//    {\
//        std::stringstream streams;\
//        streams << std::setw(90) << std::left << FUNCTION_LINE_NAME << std::string("{}");\
//        __logger__->warn(streams.str().c_str(), __VA_ARGS__);\
//        __logger__->flush();\
//    }\
//};
//
//#define LOGGER_ERROR(__logger__, ...) \
//{\
//    if(__logger__ && __logger__->should_log(spdlog::level::err))\
//    {\
//        std::stringstream streams;\
//        streams << std::setw(90) << std::left << FUNCTION_LINE_NAME << std::string("{}");\
//        __logger__->error(streams.str().c_str(), __VA_ARGS__);\
//        __logger__->flush();\
//    }\
//};
//
//#define LOGGER_CRITICAL(__logger__, ...) \
//{\
//    if(__logger__ && __logger__->should_log(spdlog::level::critical))\
//    {\
//        std::stringstream streams;\
//        streams << std::setw(90) << std::left << FUNCTION_LINE_NAME << std::string("{}");\
//        __logger__->critical(streams.str().c_str(), __VA_ARGS__);\
//        _l->flush();\
//    }\
//};
//
