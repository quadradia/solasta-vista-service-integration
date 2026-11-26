; // Event Log message table for SolastaVistaSync. Compiled by mc.exe at build time.
; // Each message is just the text the service logged (%1).

MessageIdTypedef=DWORD

SeverityNames=(Success=0x0:STATUS_SEVERITY_SUCCESS
               Informational=0x1:STATUS_SEVERITY_INFORMATIONAL
               Warning=0x2:STATUS_SEVERITY_WARNING
               Error=0x3:STATUS_SEVERITY_ERROR)

LanguageNames=(English=0x409:MSG00409)

MessageId=1000
Severity=Informational
SymbolicName=MSG_INFO
Language=English
%1
.

MessageId=1001
Severity=Warning
SymbolicName=MSG_WARNING
Language=English
%1
.

MessageId=1002
Severity=Error
SymbolicName=MSG_ERROR
Language=English
%1
.
