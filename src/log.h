#ifndef VISTA_SYNC_LOG_H
#define VISTA_SYNC_LOG_H

#include <cstdio>
#include <string>

/* Rolling daily text log (vista-sync-YYYY-MM-DD.log, old files purged) plus the
 * Windows Application event log for warnings, errors and notices. */
namespace Log {

enum Level { Debug, Info, Notice, Warning, Error };

/* dir: folder for the text files (created if missing). retentionDays: files older
 * than this are deleted. eventLog: also report to the Event Log. console: echo to the console (info to stdout, warnings and errors to stderr). */
void Init(const std::wstring& dir, int retentionDays, bool eventLog, bool console);
void Shutdown();

/* Where console echo of Debug/Info/Notice lines goes (default stdout). Warnings and errors always go to
 * stderr. --dry-run points this at stderr when stdout is redirected, so the JSON file stays clean. */
void SetConsoleInfoStream(FILE* stream);

void Write(Level level, const char* fmt, ...);

}

#define LOG_DEBUG(...) Log::Write(Log::Debug, __VA_ARGS__)
#define LOG_INFO(...) Log::Write(Log::Info, __VA_ARGS__)
/* Info that also goes to the Event Log (service start/stop, sync summaries). */
#define LOG_NOTICE(...) Log::Write(Log::Notice, __VA_ARGS__)
#define LOG_WARN(...) Log::Write(Log::Warning, __VA_ARGS__)
#define LOG_ERROR(...) Log::Write(Log::Error, __VA_ARGS__)

#endif
