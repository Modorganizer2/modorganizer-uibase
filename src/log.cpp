#include "pch.h"
#include "log.h"
#include <iostream>

#pragma warning(push)
#pragma warning(disable: 4365)
#define SPDLOG_WCHAR_FILENAMES 1
#include <spdlog/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#pragma warning(pop)


namespace MOBase::log
{

namespace fs = std::filesystem;
static std::unique_ptr<Logger> g_default;

spdlog::level::level_enum toSpdlog(Levels lv)
{
  switch (lv)
  {
    case Debug:
      return spdlog::level::debug;

    case Warning:
      return spdlog::level::warn;

    case Error:
      return spdlog::level::err;

    case Info:  // fall-through
    default:
      return spdlog::level::info;
  }
}

Levels fromSpdlog(spdlog::level::level_enum lv)
{
  switch (lv)
  {
    case spdlog::level::trace:
    case spdlog::level::debug:
      return Debug;

    case spdlog::level::warn:
      return Warning;

    case spdlog::level::critical: // fall-through
    case spdlog::level::err:
      return Error;

    case spdlog::level::info:  // fall-through
    case spdlog::level::off:
    default:
      return Info;
  }
}


class CallbackSink : public spdlog::sinks::base_sink<std::mutex>
{
public:
  CallbackSink(Callback* f)
    : m_f(f)
  {
  }

  void setCallback(Callback* f)
  {
    m_f = f;
  }

  void sink_it_(const spdlog::details::log_msg& m) override
  {
    thread_local bool active = false;

    if (active) {
      // trying to log from a log callback, ignoring
      return;
    }

    if (!m_f) {
      // disabled
      return;
    }

    try
    {
      auto g = Guard([&]{ active = false; });
      active = true;

      Entry e;
      e.time = m.time;
      e.level = fromSpdlog(m.level);
      e.message = fmt::to_string(m.payload);

      fmt::memory_buffer formatted;
      sink::formatter_->format(m, formatted);

      if (formatted.size() >= 2) {
        // remove \r\n
        e.formattedMessage.assign(formatted.begin(), formatted.end() - 2);
      } else {
        e.formattedMessage = fmt::to_string(formatted);
      }

      (*m_f)(std::move(e));
    }
    catch(std::exception& e)
    {
      fprintf(
        stderr, "uncaugh exception un logging callback, %s\n",
        e.what());
    }
    catch(...)
    {
      fprintf(stderr, "uncaught exception in logging callback\n");
    }
  }

  void flush_() override
  {
    // no-op
  }

private:
  std::atomic<Callback*> m_f;
};


File::File() :
  type(None),
  maxSize(0), maxFiles(0),
  dailyHour(0), dailyMinute(0)
{
}

File File::daily(fs::path file, int hour, int minute)
{
  File fl;

  fl.type = Daily;
  fl.file = std::move(file);
  fl.dailyHour = hour;
  fl.dailyMinute = minute;

  return fl;
}

File File::rotating(
  fs::path file, std::size_t maxSize, std::size_t maxFiles)
{
  File fl;

  fl.type = Rotating;
  fl.file = std::move(file);
  fl.maxSize = maxSize;
  fl.maxFiles = maxFiles;

  return fl;
}

spdlog::sink_ptr createFileSink(const File& f)
{
  try
  {
    switch (f.type)
    {
      case File::Daily:
      {
        return std::make_shared<spdlog::sinks::daily_file_sink_mt>(
          f.file.native(), f.dailyHour, f.dailyMinute);
      }

      case File::Rotating:
      {
        return std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          f.file.native(), f.maxSize, f.maxFiles);
      }

      case File::None:  // fall-through
      default:
        return {};
    }
  }
  catch(spdlog::spdlog_ex& e)
  {
    std::cerr << "failed to create file log, " << e.what() << "\n";
    return {};
  }
}


Logger::Logger(std::string name, Levels maxLevel, std::string pattern)
{
  createLogger(name);

  m_logger->set_level(toSpdlog(maxLevel));
  m_logger->set_pattern(pattern);
  m_logger->flush_on(spdlog::level::trace);
}

// anchor
Logger::~Logger() = default;

Levels Logger::level() const
{
  return fromSpdlog(m_logger->level());
}

void Logger::setLevel(Levels lv)
{
  m_logger->set_level(toSpdlog(lv));
}

void Logger::setPattern(const std::string& s)
{
  m_logger->set_pattern(s);
}

void Logger::setFile(const File& f)
{
  auto* ds = static_cast<spdlog::sinks::dist_sink<std::mutex>*>(m_sinks.get());

  if (m_file) {
    ds->remove_sink(m_file);
    m_file = {};
  }

  m_file = createFileSink(f);
  ds->add_sink(m_file);
}

void Logger::setCallback(Callback* f)
{
  if (m_callback) {
    static_cast<CallbackSink*>(m_callback.get())->setCallback(f);
  } else {
    auto* ds = static_cast<spdlog::sinks::dist_sink<std::mutex>*>(m_sinks.get());

    m_callback.reset(new CallbackSink(f));
    ds->add_sink(m_callback);
  }
}

void Logger::createLogger(const std::string& name)
{
  m_sinks.reset(new spdlog::sinks::dist_sink<std::mutex>);
  m_console.reset(new spdlog::sinks::stderr_color_sink_mt);

  using sink_type = spdlog::sinks::wincolor_stderr_sink_mt;

  if (auto* cs=dynamic_cast<sink_type*>(m_console.get())) {
    cs->set_color(spdlog::level::info, cs->WHITE);
    cs->set_color(spdlog::level::debug, cs->WHITE);
  }

  m_logger.reset(new spdlog::logger(name, m_sinks));
}


void createDefault(Levels maxLevel, const std::string& pattern)
{
  g_default = std::make_unique<Logger>("default", maxLevel, pattern);
}

Logger& getDefault()
{
  return *g_default;
}

} // namespace


namespace MOBase::log::details
{

void doLogImpl(spdlog::logger& lg, Levels lv, const std::string& s)
{
  const char* start = s.c_str();
  const char* p = start;

  for (;;) {
    while (*p && *p != '\n') {
      ++p;
    }

    std::string_view sv(start, static_cast<std::size_t>(p - start));
    lg.log(toSpdlog(lv), "{}", sv);

    if (!*p) {
      break;
    }

    ++p;
    start = p;
  }
}

}	// namespace