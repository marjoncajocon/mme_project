/*
** esyntax.c - syntax highlighting with VS Code's Dark+ colors
**
** One small scanner for every language: a language is a table of its
** keywords, its comments and its strings. A line is scanned from the
** state the line before left (inside a block comment, a multi-line
** string ...); a Doc keeps those states, and hl_from says from which line
** on they must be scanned again after an edit.
*/

#include "mme.h"

#include <stdlib.h>
#include <string.h>


/* states a line can start in */
enum { ST_CODE, ST_BLOCK, ST_BACKTICK, ST_TRIPLE2, ST_TRIPLE1, ST_LONG, ST_TAG };

enum { F_PREPROC = 1, F_TRIPLE = 2, F_BACKTICK = 4, F_CAPS = 8, F_UPPER = 16,
       F_SHELL = 32, F_JSON = 64, F_MD = 128, F_LUA = 256, F_DOLLAR = 512,
       F_NOCASE = 1024, F_CSS = 2048,
       F_ICASE = 4096,	/* keywords in any case (SQL, Dockerfile) */
       F_PLAIN = 8192,	/* other words are plain text */
       F_MARKUP = 16384,	/* HTML, XML: <tags attr="x"> */
       F_INI = 32768,	/* [section], key = value */
       F_DIFF = 65536,	/* + and - lines */
       F_SIGIL = 131072,	/* $var ${var} @var */
       F_ASM = 262144,	/* the first word is an instruction, label: */
       F_TEX = 524288,	/* \commands */
       F_ATOM = 1048576,	/* :symbols */
       F_LEAD = 2097152 };	/* keywords only as a line's first word (Dockerfile) */

struct Syntax {
  const char *name;
  const char *id;	/* VS Code's language id: "c", "csharp", "shellscript" ... */
  const char *files;	/* " .c .h makefile ": extensions and names */
  const char *const *control;	/* purple: if, return ... */
  const char *const *storage;	/* blue: int, static, true ... */
  const char *const *types;	/* teal: size_t, string ... */
  const char *line;	/* a comment to the end of the line */
  const char *open, *close;	/* a block comment */
  const char *quotes;
  int flags;
};


/*
** {==================================================================
** Languages
** ===================================================================
*/

static const char *const c_control[] = {
  "if", "else", "for", "while", "do", "switch", "case", "default", "break",
  "continue", "return", "goto", "try", "catch", "throw", "new", "delete", NULL
};
static const char *const c_storage[] = {
  "int", "char", "short", "long", "float", "double", "void", "unsigned",
  "signed", "static", "const", "extern", "struct", "union", "enum", "typedef",
  "sizeof", "volatile", "register", "inline", "restrict", "auto", "_Bool",
  "bool", "true", "false", "NULL", "nullptr", "class", "public", "private",
  "protected", "virtual", "template", "typename", "namespace", "using",
  "this", "operator", "constexpr", "noexcept", "override", "_Noreturn",
  "_Static_assert", "_Alignas", "_Alignof", "_Atomic", "_Thread_local", NULL
};
static const char *const c_types[] = {
  "size_t", "ssize_t", "ptrdiff_t", "intptr_t", "uintptr_t", "int8_t",
  "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
  "uint64_t", "wchar_t", "FILE", "va_list", "time_t", "off_t", "pid_t",
  "std", "string", "vector", NULL
};

static const char *const js_control[] = {
  "if", "else", "for", "while", "do", "switch", "case", "default", "break",
  "continue", "return", "try", "catch", "finally", "throw", "await", "yield",
  "import", "export", "from", "as", "of", "in", NULL
};
static const char *const js_storage[] = {
  "var", "let", "const", "function", "class", "extends", "new", "delete",
  "typeof", "instanceof", "void", "this", "super", "true", "false", "null",
  "undefined", "async", "static", "get", "set", "interface", "type", "enum",
  "implements", "public", "private", "protected", "readonly", "declare",
  "namespace", "keyof", "abstract", NULL
};
static const char *const js_types[] = {
  "string", "number", "boolean", "any", "unknown", "never", "object",
  "Promise", "Array", "Map", "Set", "Record", NULL
};

static const char *const go_control[] = {
  "if", "else", "for", "range", "switch", "case", "default", "break",
  "continue", "return", "goto", "go", "defer", "select", "fallthrough",
  "import", "package", NULL
};
static const char *const go_storage[] = {
  "func", "var", "const", "type", "struct", "interface", "map", "chan",
  "true", "false", "nil", "iota", NULL
};
static const char *const go_types[] = {
  "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16",
  "uint32", "uint64", "uintptr", "float32", "float64", "complex64",
  "complex128", "byte", "rune", "string", "bool", "error", "any", NULL
};

static const char *const py_control[] = {
  "if", "elif", "else", "for", "while", "break", "continue", "return", "try",
  "except", "finally", "raise", "with", "yield", "import", "from", "as",
  "pass", "assert", "await", "in", "is", "not", "and", "or", "match", "case",
  NULL
};
static const char *const py_storage[] = {
  "def", "class", "lambda", "global", "nonlocal", "del", "True", "False",
  "None", "self", "async", NULL
};
static const char *const py_types[] = {
  "int", "float", "str", "bool", "list", "dict", "set", "tuple", "bytes",
  "object", NULL
};

static const char *const rs_control[] = {
  "if", "else", "for", "while", "loop", "match", "break", "continue",
  "return", "in", "use", "mod", "as", "await", NULL
};
static const char *const rs_storage[] = {
  "fn", "let", "mut", "const", "static", "struct", "enum", "trait", "impl",
  "pub", "crate", "self", "Self", "super", "where", "type", "ref", "move",
  "unsafe", "async", "dyn", "true", "false", "extern", NULL
};
static const char *const rs_types[] = {
  "i8", "i16", "i32", "i64", "i128", "isize", "u8", "u16", "u32", "u64",
  "u128", "usize", "f32", "f64", "bool", "char", "str", "String", "Vec",
  "Option", "Result", "Box", NULL
};

static const char *const zig_control[] = {
  "if", "else", "for", "while", "switch", "break", "continue", "return",
  "try", "catch", "orelse", "defer", "errdefer", "unreachable", NULL
};
static const char *const zig_storage[] = {
  "fn", "const", "var", "pub", "struct", "enum", "union", "error", "comptime",
  "inline", "export", "extern", "true", "false", "null", "undefined", "test",
  NULL
};
static const char *const zig_types[] = {
  "u8", "u16", "u32", "u64", "usize", "i8", "i16", "i32", "i64", "isize",
  "f32", "f64", "bool", "void", "type", "anytype", "noreturn", NULL
};

static const char *const java_control[] = {
  "if", "else", "for", "while", "do", "switch", "case", "default", "break",
  "continue", "return", "try", "catch", "finally", "throw", "throws", "import",
  "package", "assert", "yield", NULL
};
static const char *const java_storage[] = {
  "class", "interface", "enum", "extends", "implements", "new", "public",
  "private", "protected", "static", "final", "abstract", "void", "int",
  "long", "short", "byte", "char", "float", "double", "boolean", "this",
  "super", "true", "false", "null", "var", "record", "synchronized",
  "transient", "volatile", "native", "instanceof", "sealed", "permits", NULL
};
static const char *const java_types[] = {
  "String", "Integer", "Long", "Double", "Boolean", "Object", "List", "Map",
  "Set", "ArrayList", "HashMap", NULL
};

static const char *const kt_control[] = {
  "if", "else", "for", "while", "do", "when", "break", "continue", "return",
  "try", "catch", "finally", "throw", "import", "package", "in", "is", "as",
  NULL
};
static const char *const kt_storage[] = {
  "fun", "val", "var", "class", "interface", "object", "enum", "data",
  "sealed", "open", "abstract", "override", "private", "public", "protected",
  "internal", "companion", "lateinit", "suspend", "inline", "true", "false",
  "null", "this", "super", "typealias", "const", "by", "init", "constructor",
  NULL
};
static const char *const kt_types[] = {
  "Int", "Long", "Short", "Byte", "Double", "Float", "Boolean", "Char",
  "String", "Unit", "Any", "Nothing", "List", "Map", "Set", "Array", NULL
};

static const char *const cs_control[] = {
  "if", "else", "for", "foreach", "while", "do", "switch", "case", "default",
  "break", "continue", "return", "try", "catch", "finally", "throw", "goto",
  "using", "yield", "await", "in", "is", "as", "when", NULL
};
static const char *const cs_storage[] = {
  "class", "struct", "interface", "enum", "record", "namespace", "new",
  "public", "private", "protected", "internal", "static", "readonly", "const",
  "virtual", "override", "abstract", "sealed", "async", "partial", "var",
  "void", "int", "long", "short", "byte", "char", "float", "double",
  "decimal", "bool", "string", "object", "dynamic", "this", "base", "true",
  "false", "null", "ref", "out", "params", "get", "set", "init", "typeof",
  "nameof", "sizeof", "delegate", "event", "operator", "implicit",
  "explicit", "unsafe", "fixed", "lock", "checked", NULL
};
static const char *const cs_types[] = {
  "String", "Int32", "Int64", "Boolean", "Object", "List", "Dictionary",
  "Task", "IEnumerable", "Console", "Exception", NULL
};

static const char *const swift_control[] = {
  "if", "else", "for", "while", "repeat", "switch", "case", "default",
  "break", "continue", "return", "guard", "defer", "do", "try", "catch",
  "throw", "throws", "rethrows", "import", "in", "where", "fallthrough",
  "await", NULL
};
static const char *const swift_storage[] = {
  "func", "let", "var", "class", "struct", "enum", "protocol", "extension",
  "init", "deinit", "self", "Self", "super", "true", "false", "nil",
  "public", "private", "fileprivate", "internal", "open", "static", "final",
  "override", "mutating", "inout", "typealias", "associatedtype", "async",
  "some", "any", "lazy", "weak", "unowned", "subscript", "operator", NULL
};
static const char *const swift_types[] = {
  "Int", "Double", "Float", "Bool", "String", "Character", "Array",
  "Dictionary", "Set", "Optional", "Any", "Void", NULL
};

static const char *const dart_control[] = {
  "if", "else", "for", "while", "do", "switch", "case", "default", "break",
  "continue", "return", "try", "catch", "finally", "throw", "rethrow",
  "import", "export", "library", "part", "as", "show", "hide", "in", "is",
  "await", "yield", "assert", NULL
};
static const char *const dart_storage[] = {
  "class", "enum", "extends", "implements", "with", "mixin", "extension",
  "new", "const", "final", "var", "late", "static", "abstract", "factory",
  "void", "int", "double", "num", "bool", "dynamic", "this", "super",
  "true", "false", "null", "async", "required", "typedef", "get", "set",
  "operator", "covariant", NULL
};
static const char *const dart_types[] = {
  "String", "List", "Map", "Set", "Future", "Stream", "Object", "Widget",
  "Iterable", NULL
};

static const char *const php_control[] = {
  "if", "else", "elseif", "endif", "for", "foreach", "endforeach", "while",
  "endwhile", "do", "switch", "case", "default", "break", "continue",
  "return", "try", "catch", "finally", "throw", "match", "require",
  "require_once", "include", "include_once", "use", "namespace", "as",
  "yield", "and", "or", "not", NULL
};
static const char *const php_storage[] = {
  "function", "fn", "class", "interface", "trait", "enum", "extends",
  "implements", "new", "public", "private", "protected", "static", "final",
  "abstract", "const", "var", "global", "echo", "print", "true", "false",
  "null", "TRUE", "FALSE", "NULL", "array", "isset", "unset", "empty",
  "list", "instanceof", "readonly", NULL
};
static const char *const php_types[] = {
  "int", "float", "string", "bool", "mixed", "void", "object", "callable",
  "iterable", "self", "parent", NULL
};

static const char *const rb_control[] = {
  "if", "elsif", "else", "unless", "case", "when", "while", "until", "for",
  "in", "do", "end", "begin", "rescue", "ensure", "raise", "return",
  "break", "next", "redo", "retry", "yield", "then", "and", "or", "not",
  "require", "require_relative", "include", "extend", NULL
};
static const char *const rb_storage[] = {
  "def", "class", "module", "self", "super", "true", "false", "nil",
  "attr_accessor", "attr_reader", "attr_writer", "private", "public",
  "protected", "alias", "lambda", "proc", "defined?", NULL
};

static const char *const sql_control[] = {
  "select", "from", "where", "and", "or", "not", "insert", "into", "values",
  "update", "set", "delete", "create", "table", "drop", "alter", "add",
  "join", "inner", "left", "right", "outer", "full", "cross", "on", "as",
  "group", "by", "order", "having", "limit", "offset", "union", "all",
  "distinct", "case", "when", "then", "else", "end", "in", "is", "like",
  "between", "exists", "primary", "key", "foreign", "references", "index",
  "view", "begin", "commit", "rollback", "transaction", "with", "returning",
  "default", "unique", "check", "constraint", "if", "asc", "desc", "grant",
  "revoke", "trigger", "procedure", "function", "return", "returns",
  "declare", NULL
};
static const char *const sql_storage[] = {
  "null", "true", "false", NULL
};
static const char *const sql_types[] = {
  "int", "integer", "bigint", "smallint", "tinyint", "serial", "decimal",
  "numeric", "real", "float", "double", "varchar", "char", "text", "blob",
  "boolean", "bool", "date", "time", "timestamp", "datetime", "json",
  "jsonb", "uuid", NULL
};

static const char *const docker_control[] = {
  "from", "run", "cmd", "label", "maintainer", "expose", "env", "add", "copy",
  "entrypoint", "volume", "user", "workdir", "arg", "onbuild", "stopsignal",
  "healthcheck", "shell", "as", NULL
};

static const char *const hs_control[] = {
  "if", "then", "else", "case", "of", "let", "in", "where", "do", "import",
  "module", "qualified", "hiding", "as", "deriving", NULL
};
static const char *const hs_storage[] = {
  "data", "type", "newtype", "class", "instance", "True", "False",
  "Nothing", "Just", "forall", "infixl", "infixr", "infix", NULL
};
static const char *const hs_types[] = {
  "Int", "Integer", "Double", "Float", "Bool", "Char", "String", "IO",
  "Maybe", "Either", NULL
};

static const char *const ex_control[] = {
  "if", "else", "unless", "cond", "case", "when", "with", "for", "do", "end",
  "fn", "try", "catch", "rescue", "after", "raise", "receive", "import",
  "alias", "require", "use", "and", "or", "not", "in", NULL
};
static const char *const ex_storage[] = {
  "def", "defp", "defmodule", "defmacro", "defmacrop", "defstruct",
  "defprotocol", "defimpl", "defdelegate", "defguard", "true", "false",
  "nil", "__MODULE__", NULL
};

static const char *const erl_control[] = {
  "if", "case", "of", "end", "receive", "after", "when", "fun", "try",
  "catch", "throw", "begin", "and", "andalso", "or", "orelse", "not",
  "xor", "band", "bor", "div", "rem", NULL
};
static const char *const erl_storage[] = {
  "true", "false", "undefined", "ok", "error", NULL
};

static const char *const r_control[] = {
  "if", "else", "for", "while", "repeat", "break", "next", "return",
  "function", "in", "library", "require", "switch", NULL
};
static const char *const r_storage[] = {
  "TRUE", "FALSE", "NULL", "NA", "Inf", "NaN", "T", "F", NULL
};

static const char *const pl_control[] = {
  "if", "elsif", "else", "unless", "while", "until", "for", "foreach", "do",
  "last", "next", "redo", "return", "use", "require", "package", "and",
  "or", "not", "eq", "ne", "lt", "gt", "le", "ge", NULL
};
static const char *const pl_storage[] = {
  "sub", "my", "our", "local", "print", "printf", "die", "warn", "undef",
  "bless", "ref", "shift", "push", "pop", NULL
};

static const char *const scala_control[] = {
  "if", "else", "for", "while", "do", "match", "case", "try", "catch",
  "finally", "throw", "return", "yield", "import", "package", "then", NULL
};
static const char *const scala_storage[] = {
  "def", "val", "var", "class", "object", "trait", "extends", "with", "new",
  "private", "protected", "override", "abstract", "final", "sealed",
  "implicit", "lazy", "given", "using", "enum", "type", "this", "super",
  "true", "false", "null", NULL
};

static const char *const nim_control[] = {
  "if", "elif", "else", "for", "while", "case", "of", "break", "continue",
  "return", "try", "except", "finally", "raise", "import", "from",
  "include", "export", "when", "block", "yield", "discard", "and", "or",
  "not", "in", "notin", "is", "isnot", "defer", NULL
};
static const char *const nim_storage[] = {
  "proc", "func", "method", "iterator", "macro", "template", "var", "let",
  "const", "type", "object", "enum", "tuple", "ref", "ptr", "distinct",
  "true", "false", "nil", "result", NULL
};
static const char *const nim_types[] = {
  "int", "int8", "int16", "int32", "int64", "uint", "uint8", "float",
  "float32", "float64", "bool", "char", "string", "seq", "array", NULL
};

static const char *const odin_control[] = {
  "if", "else", "for", "in", "switch", "case", "break", "continue", "return",
  "defer", "when", "fallthrough", "import", "package", "foreign", "do",
  "or_else", "or_return", NULL
};
static const char *const odin_storage[] = {
  "proc", "struct", "enum", "union", "bit_set", "map", "dynamic", "distinct",
  "using", "context", "true", "false", "nil", "cast", "transmute",
  "auto_cast", "size_of", "align_of", "typeid_of", NULL
};
static const char *const odin_types[] = {
  "int", "uint", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32",
  "f64", "bool", "string", "cstring", "rune", "rawptr", "any", "typeid",
  "byte", "uintptr", NULL
};

static const char *const v_control[] = {
  "if", "else", "for", "in", "match", "break", "continue", "return",
  "defer", "go", "spawn", "import", "module", "or", "select", "unsafe",
  "assert", "lock", "rlock", NULL
};
static const char *const v_storage[] = {
  "fn", "mut", "pub", "struct", "enum", "interface", "type", "const",
  "union", "true", "false", "none", "shared", "static", "__global", NULL
};

static const char *const jl_control[] = {
  "if", "elseif", "else", "end", "for", "while", "break", "continue",
  "return", "try", "catch", "finally", "do", "begin", "let", "in", "using",
  "import", "export", NULL
};
static const char *const jl_storage[] = {
  "function", "macro", "struct", "mutable", "abstract", "primitive", "type",
  "module", "baremodule", "const", "global", "local", "quote", "true",
  "false", "nothing", "missing", NULL
};

static const char *const objc_storage[] = {
  "int", "char", "short", "long", "float", "double", "void", "unsigned",
  "signed", "static", "const", "extern", "struct", "union", "enum",
  "typedef", "sizeof", "inline", "bool", "true", "false", "NULL",
  "interface", "implementation", "end", "property", "synthesize",
  "dynamic", "protocol", "optional", "required", "class", "selector",
  "autoreleasepool", "import", "self", "super", "nil", "Nil", "YES", "NO",
  "id", "BOOL", "nonatomic", "atomic", "strong", "weak", "copy", "assign",
  "readonly", "readwrite", "instancetype", NULL
};

static const char *const asm_regs[] = {
  "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10",
  "r11", "r12", "r13", "r14", "r15", "eax", "ebx", "ecx", "edx", "esi",
  "edi", "ebp", "esp", "ax", "bx", "cx", "dx", "al", "bl", "cl", "dl", "ah",
  "bh", "ch", "dh", "si", "di", "sp", "bp", "rip", "eip", "cs", "ds", "es",
  "fs", "gs", "ss", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
  "xmm7", "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x29", "x30",
  "w0", "w1", "w2", "w3", "sp", "lr", "pc", "byte", "word", "dword",
  "qword", "ptr", NULL
};

static const char *const cmake_control[] = {
  "if", "elseif", "else", "endif", "foreach", "endforeach", "while",
  "endwhile", "function", "endfunction", "macro", "endmacro", "return",
  "break", "continue", "include", NULL
};
static const char *const cmake_storage[] = {
  "on", "off", "true", "false", "yes", "no", NULL
};

static const char *const gql_control[] = {
  "query", "mutation", "subscription", "fragment", "on", "schema", "extend",
  "implements", "directive", NULL
};
static const char *const gql_storage[] = {
  "type", "interface", "union", "enum", "input", "scalar", "true", "false",
  "null", NULL
};
static const char *const gql_types[] = {
  "Int", "Float", "String", "Boolean", "ID", NULL
};

static const char *const proto_control[] = {
  "syntax", "import", "package", "option", "returns", "rpc", "stream",
  "public", "weak", "reserved", "to", "max", "extend", "edition", NULL
};
static const char *const proto_storage[] = {
  "message", "enum", "service", "oneof", "map", "repeated", "optional",
  "required", "true", "false", NULL
};
static const char *const proto_types[] = {
  "double", "float", "int32", "int64", "uint32", "uint64", "sint32",
  "sint64", "fixed32", "fixed64", "sfixed32", "sfixed64", "bool", "string",
  "bytes", NULL
};

static const char *const tf_control[] = {
  "for", "in", "if", "for_each", "count", "depends_on", "dynamic",
  "content", "lifecycle", NULL
};
static const char *const tf_storage[] = {
  "resource", "data", "variable", "output", "module", "provider", "locals",
  "terraform", "backend", "true", "false", "null", "required_providers",
  NULL
};
static const char *const tf_types[] = {
  "string", "number", "bool", "list", "map", "set", "object", "tuple", "any",
  NULL
};

static const char *const log_control[] = {
  "error", "err", "fatal", "fail", "failed", "failure", "critical", "crit",
  "exception", "panic", "emerg", "alert", "severe", NULL
};
static const char *const log_storage[] = {
  "info", "information", "notice", "debug", "dbg", "trace", "verbose",
  "true", "false", "null", "ok", "success", NULL
};
static const char *const log_types[] = {
  "warn", "warning", "wrn", NULL
};

static const char *const lua_control[] = {
  "if", "then", "else", "elseif", "end", "for", "while", "do", "repeat",
  "until", "break", "return", "goto", "in", "and", "or", "not", NULL
};
static const char *const lua_storage[] = {
  "local", "function", "true", "false", "nil", "self", NULL
};

static const char *const sh_control[] = {
  "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done",
  "case", "esac", "in", "return", "break", "continue", "exit", "select",
  NULL
};
static const char *const sh_storage[] = {
  "function", "local", "export", "readonly", "declare", "alias", "source",
  "unset", "shift", "set", "true", "false", "echo", "cd", "test", NULL
};

static const char *const bat_control[] = {
  "if", "else", "for", "in", "do", "goto", "call", "exit", "not", "exist",
  "defined", NULL
};
static const char *const bat_storage[] = {
  "set", "echo", "setlocal", "endlocal", "rem", "cd", "copy", "del", "mkdir",
  "rmdir", "where", "shift", "pause", "off", "on", NULL
};

static const char *const json_storage[] = {"true", "false", "null", NULL};

static const char *const css_storage[] = {
  "important", "inherit", "initial", "none", "auto", NULL
};

static const Syntax langs[] = {
  {"C", "c", " .c .h ", c_control, c_storage, c_types, "//", "/*", "*/", "\"'",
   F_PREPROC | F_CAPS | F_UPPER},
  {"C++", "cpp", " .cpp .cc .cxx .c++ .hpp .hh .hxx .h++ .ipp .inl .tpp ", c_control, c_storage,
   c_types, "//", "/*", "*/", "\"'", F_PREPROC | F_CAPS | F_UPPER},
  {"C#", "csharp", " .cs .csx ", cs_control, cs_storage, cs_types, "//", "/*", "*/", "\"'",
   F_PREPROC | F_UPPER},
  {"Objective-C", "objective-c", " .m .mm ", c_control, objc_storage, c_types, "//", "/*",
   "*/", "\"'", F_PREPROC | F_CAPS | F_UPPER},
  {"Java", "java", " .java .jav ", java_control, java_storage, java_types, "//", "/*", "*/",
   "\"'", F_CAPS | F_UPPER},
  {"Kotlin", "kotlin", " .kt .kts ", kt_control, kt_storage, kt_types, "//", "/*", "*/",
   "\"'", F_TRIPLE | F_CAPS | F_UPPER | F_DOLLAR},
  {"Scala", "scala", " .scala .sc .sbt ", scala_control, scala_storage, NULL, "//", "/*", "*/",
   "\"'", F_TRIPLE | F_CAPS | F_UPPER},
  {"Swift", "swift", " .swift ", swift_control, swift_storage, swift_types, "//", "/*", "*/",
   "\"", F_TRIPLE | F_CAPS | F_UPPER},
  {"Dart", "dart", " .dart ", dart_control, dart_storage, dart_types, "//", "/*", "*/", "\"'",
   F_TRIPLE | F_CAPS | F_UPPER},
  {"JavaScript", "javascript", " .js .mjs .cjs .jsx .es6 ", js_control, js_storage, js_types,
   "//", "/*", "*/", "\"'`", F_BACKTICK | F_CAPS | F_UPPER | F_DOLLAR},
  {"TypeScript", "typescript", " .ts .tsx .mts .cts ", js_control, js_storage, js_types, "//",
   "/*", "*/", "\"'`", F_BACKTICK | F_CAPS | F_UPPER | F_DOLLAR},
  {"Go", "go", " .go ", go_control, go_storage, go_types, "//", "/*", "*/", "\"'`",
   F_BACKTICK | F_UPPER},
  {"Python", "python", " .py .pyw .pyi .gyp sconstruct sconscript ", py_control, py_storage,
   py_types, "#", NULL, NULL, "\"'", F_TRIPLE | F_CAPS | F_UPPER},
  {"Rust", "rust", " .rs ", rs_control, rs_storage, rs_types, "//", "/*", "*/", "\"",
   F_CAPS | F_UPPER},
  {"Zig", "zig", " .zig .zon ", zig_control, zig_storage, zig_types, "//", NULL, NULL,
   "\"'", F_CAPS | F_UPPER},
  {"Odin", "odin", " .odin ", odin_control, odin_storage, odin_types, "//", "/*", "*/",
   "\"'`", F_BACKTICK | F_CAPS | F_UPPER},
  {"V", "v", " .v .vsh ", v_control, v_storage, go_types, "//", "/*", "*/", "\"'`",
   F_BACKTICK | F_CAPS | F_UPPER},
  {"Nim", "nim", " .nim .nims .nimble ", nim_control, nim_storage, nim_types, "#", "#[", "]#",
   "\"'", F_TRIPLE | F_UPPER},
  {"Julia", "julia", " .jl ", jl_control, jl_storage, NULL, "#", "#=", "=#", "\"'",
   F_TRIPLE | F_UPPER},
  {"Lua", "lua", " .lua .luau .rockspec ", lua_control, lua_storage, NULL, "--", NULL, NULL,
   "\"'", F_LUA | F_CAPS},
  {"PHP", "php", " .php .phtml .php3 .php4 .php5 .phps ", php_control, php_storage, php_types,
   "//", "/*", "*/", "\"'`", F_SIGIL | F_UPPER},
  {"Ruby", "ruby", " .rb .rbw .rake .gemspec .ru .erb rakefile gemfile guardfile podfile "
   "vagrantfile ", rb_control, rb_storage, NULL, "#", NULL, NULL, "\"'`",
   F_SIGIL | F_ATOM | F_CAPS | F_UPPER},
  {"Perl", "perl", " .pl .pm .pod .t .psgi ", pl_control, pl_storage, NULL, "#", NULL, NULL,
   "\"'`", F_SIGIL},
  {"Elixir", "elixir", " .ex .exs ", ex_control, ex_storage, NULL, "#", NULL, NULL, "\"'",
   F_TRIPLE | F_ATOM | F_UPPER},
  {"Erlang", "erlang", " .erl .hrl .escript rebar.config ", erl_control, erl_storage, NULL, "%",
   NULL, NULL, "\"", F_UPPER},
  {"Haskell", "haskell", " .hs .lhs ", hs_control, hs_storage, hs_types, "--", "{-", "-}", "\"",
   F_UPPER},
  {"R", "r", " .r .rhistory .rprofile ", r_control, r_storage, NULL, "#", NULL, NULL, "\"'",
   0},
  {"Java Properties", "properties", " .properties .env .env.local .env.example "
   ".env.development .env.production .gitattributes ", NULL, NULL, NULL, "#", NULL, NULL, NULL,
   F_INI | F_PLAIN},
  {"Shell Script", "shellscript", " .sh .bash .zsh .ksh .fish .mmcrc .bashrc .zshrc .bash_profile "
   ".bash_aliases .profile profile pkgbuild .envrc ", sh_control, sh_storage, NULL, "#", NULL,
   NULL, "\"'", F_SHELL},
  {"Batch", "bat", " .bat .cmd ", bat_control, bat_storage, NULL, "::", NULL, NULL, "\"",
   F_NOCASE},
  {"PowerShell", "powershell", " .ps1 .psm1 .psd1 ", sh_control, sh_storage, NULL, "#", "<#",
   "#>", "\"'", F_SHELL | F_NOCASE},
  {"Dockerfile", "dockerfile", " dockerfile containerfile .dockerfile ", docker_control, NULL,
   NULL, "#", NULL, NULL, "\"'", F_SHELL | F_ICASE | F_LEAD},
  {"Makefile", "makefile", " makefile gnumakefile .mk .mak ", NULL, NULL, NULL, "#", NULL, NULL,
   "\"'", F_SHELL},
  {"CMake", "cmake", " cmakelists.txt .cmake ", cmake_control, cmake_storage, NULL, "#", NULL,
   NULL, "\"", F_ICASE | F_PLAIN | F_SIGIL | F_CAPS},
  {"SQL", "sql", " .sql .ddl .dml .psql .pgsql ", sql_control, sql_storage, sql_types, "--",
   "/*", "*/", "'\"`", F_ICASE | F_PLAIN},
  {"JSON", "json", " .json .webmanifest .har .geojson .jsonl .ndjson .babelrc .prettierrc ",
   NULL, json_storage, NULL, "//", "/*", "*/", "\"", F_JSON},
  {"JSON with Comments", "jsonc", " .jsonc .code-workspace .code-snippets settings.json "
   "launch.json tasks.json keybindings.json extensions.json tsconfig.json jsconfig.json "
   ".eslintrc.json .eslintrc devcontainer.json .devcontainer.json ", NULL, json_storage, NULL,
   "//", "/*", "*/", "\"", F_JSON},
  {"YAML", "yaml", " .yml .yaml .clang-format .clangd ", NULL, json_storage, NULL, "#", NULL,
   NULL, "\"'", 0},
  {"TOML", "toml", " .toml pipfile ", NULL, json_storage, NULL, "#", NULL, NULL, "\"'",
   F_INI | F_PLAIN},
  {"Ini", "ini", " .ini .cfg .conf .cnf .editorconfig .gitconfig .gitmodules .npmrc .wslconfig "
   ".desktop .service .reg .inf ", NULL, json_storage, NULL, ";", NULL, NULL, "\"",
   F_INI | F_PLAIN},
  {"Ignore", "ignore", " .gitignore .dockerignore .npmignore .eslintignore .prettierignore "
   ".hgignore .vscodeignore ", NULL, NULL, NULL, "#", NULL, NULL, NULL, F_PLAIN},
  {"Markdown", "markdown", " .md .markdown .mdown .mkd .mdx ", NULL, NULL, NULL, NULL, "<!--",
   "-->", "`", F_MD},
  {"LaTeX", "latex", " .tex .sty .cls .ltx .bib ", NULL, NULL, NULL, "%", NULL, NULL, "$",
   F_TEX | F_PLAIN},
  {"CSS", "css", " .css ", NULL, css_storage, NULL, NULL, "/*", "*/", "\"'", F_CSS},
  {"SCSS", "scss", " .scss .sass ", NULL, css_storage, NULL, "//", "/*", "*/", "\"'",
   F_CSS | F_SIGIL},
  {"Less", "less", " .less ", NULL, css_storage, NULL, "//", "/*", "*/", "\"'", F_CSS},
  {"HTML", "html", " .html .htm .xhtml .shtml .jsp .asp .aspx .cshtml .razor .hbs "
   ".handlebars .njk .liquid ", NULL, NULL, NULL, NULL, "<!--", "-->", "\"'", F_MARKUP},
  {"XML", "xml", " .xml .svg .xaml .xsd .xsl .xslt .plist .csproj .fsproj .vbproj .vcxproj "
   ".props .targets .resx .nuspec .wsdl .rss .atom .kml .gpx .storyboard .xib .iml "
   ".tmtheme .tmlanguage .manifest ", NULL, NULL, NULL, NULL, "<!--", "-->", "\"'", F_MARKUP},
  {"Vue", "vue", " .vue ", NULL, NULL, NULL, NULL, "<!--", "-->", "\"'", F_MARKUP},
  {"Svelte", "svelte", " .svelte ", NULL, NULL, NULL, NULL, "<!--", "-->", "\"'", F_MARKUP},
  {"GraphQL", "graphql", " .graphql .gql .graphqls ", gql_control, gql_storage, gql_types, "#",
   NULL, NULL, "\"", F_TRIPLE | F_UPPER},
  {"Protocol Buffers", "proto", " .proto ", proto_control, proto_storage, proto_types, "//",
   "/*", "*/", "\"'", F_UPPER},
  {"Terraform", "terraform", " .tf .tfvars .hcl .nomad ", tf_control, tf_storage, tf_types,
   "#", "/*", "*/", "\"", F_PLAIN},
  {"Assembly", "asm", " .asm .nasm .inc .s .S .a51 ", NULL, NULL, asm_regs, ";", NULL, NULL,
   "\"'", F_ASM | F_ICASE | F_PLAIN},
  {"Diff", "diff", " .diff .patch .rej ", NULL, NULL, NULL, NULL, NULL, NULL, NULL, F_DIFF},
  {"Log", "log", " .log ", log_control, log_storage, log_types, NULL, NULL, NULL, "\"",
   F_ICASE | F_PLAIN},
  {"Resource", "rc", " .rc ", c_control, c_storage, c_types, "//", "/*", "*/", "\"",
   F_PREPROC | F_CAPS}
};


/* is word one of the space separated words of list? (case does not matter) */
static int listed (const char *list, const char *word) {
  size_t n = strlen(word);
  const char *p;
  for (p = list; *p; p++)
    if (p > list && p[-1] == ' ' && m_strnicmp(p, word, n) == 0 && p[n] == ' ') return 1;
  return 0;
}


const Syntax *syntax_for (const char *name) {
  const char *base = path_basename(name), *dot = strrchr(base, '.');
  size_t i, n = sizeof(langs) / sizeof(langs[0]);
  for (i = 0; i < n; i++)	/* the whole name first: makefile, .bashrc */
    if (listed(langs[i].files, base)) return &langs[i];
  if (m_strnicmp(base, "dockerfile.", 11) == 0 || m_strnicmp(base, "containerfile.", 14) == 0)
    return syntax_by_id("dockerfile");	/* Dockerfile.dev */
  if (m_strnicmp(base, ".env.", 5) == 0) return syntax_by_id("properties");	/* .env.test */
  if (m_strnicmp(base, "makefile.", 9) == 0) return syntax_by_id("makefile");
  if (dot == NULL) return NULL;
  for (i = 0; i < n; i++)
    if (listed(langs[i].files, dot)) return &langs[i];
  return NULL;
}


static Syntax *g_extra[128];	/* languages VS Code's extensions brought */
static int g_nextra;


/* the language with VS Code's id ("go", "csharp" ...); NULL: none */
const Syntax *syntax_by_id (const char *id) {
  size_t i, n = sizeof(langs) / sizeof(langs[0]);
  int k;
  for (i = 0; id && i < n; i++)
    if (strcmp(langs[i].id, id) == 0) return &langs[i];
  for (k = 0; id && k < g_nextra; k++)
    if (strcmp(g_extra[k]->id, id) == 0) return g_extra[k];
  return NULL;
}


/* a language only an extension knows: a Syntax for it (no keywords: its grammar colors it) */
const Syntax *syntax_extra (const char *name, const char *id, const char *line, const char *open, const char *close) {
  Syntax *s;
  int k;
  for (k = 0; k < g_nextra; k++)
    if (strcmp(g_extra[k]->id, id) == 0) return g_extra[k];
  if (g_nextra == 128) return NULL;
  s = (Syntax *)calloc(1, sizeof(Syntax));
  s->name = xstrdup(name);
  s->id = xstrdup(id);
  s->files = "";
  s->line = line ? xstrdup(line) : NULL;
  s->open = open && close ? xstrdup(open) : NULL;
  s->close = open && close ? xstrdup(close) : NULL;
  s->quotes = "\"'";
  g_extra[g_nextra++] = s;
  return s;
}


const char *syntax_lang (const Syntax *sx) {
  return sx ? sx->id : NULL;
}


/* VS Code's language id of a language's name ("C#" -> "csharp"); NULL: not one of mme's */
const char *syntax_id (const char *name) {
  size_t i, n = sizeof(langs) / sizeof(langs[0]);
  int k;
  for (i = 0; name && i < n; i++)
    if (strcmp(langs[i].name, name) == 0) return langs[i].id;
  for (k = 0; name && k < g_nextra; k++)
    if (strcmp(g_extra[k]->name, name) == 0) return g_extra[k]->id;
  return NULL;
}


/*
** A file without a known name by its first line, like VS Code's
** firstLine: "#!/bin/sh", "#!/usr/bin/env python3", "<?php", "<?xml".
*/
const Syntax *syntax_first_line (const char *s, size_t n) {
  static const struct {
    const char *prog, *id;
  } progs[] = {
    {"sh", "shellscript"}, {"bash", "shellscript"}, {"zsh", "shellscript"},
    {"dash", "shellscript"}, {"ksh", "shellscript"}, {"ash", "shellscript"},
    {"fish", "shellscript"}, {"mmc", "shellscript"}, {"python", "python"},
    {"pypy", "python"}, {"node", "javascript"}, {"nodejs", "javascript"},
    {"deno", "typescript"}, {"bun", "javascript"}, {"ts-node", "typescript"},
    {"ruby", "ruby"}, {"perl", "perl"}, {"php", "php"}, {"lua", "lua"},
    {"luajit", "lua"}, {"Rscript", "r"}, {"julia", "julia"}, {"elixir", "elixir"},
    {"escript", "erlang"}, {"pwsh", "powershell"}, {"powershell", "powershell"},
    {"make", "makefile"}, {"runhaskell", "haskell"}, {"scala", "scala"},
    {"swift", "swift"}, {"dart", "dart"}, {"kotlin", "kotlin"}, {"nim", "nim"},
    {"v", "v"}
  };
  size_t i = 2, w, k;
  char prog[32];
  if (n >= 5 && memcmp(s, "<?php", 5) == 0) return syntax_by_id("php");
  if (n >= 5 && memcmp(s, "<?xml", 5) == 0) return syntax_by_id("xml");
  if (n >= 9 && m_strnicmp(s, "<!doctype", 9) == 0) return syntax_by_id("html");
  if (n < 3 || s[0] != '#' || s[1] != '!') return NULL;
  for (;;) {	/* the program: the last part of the path, or after env */
    while (i < n && s[i] == ' ') i++;
    w = i;
    while (i < n && s[i] != ' ' && s[i] != '\t') i++;
    for (k = i; k > w && s[k - 1] != '/' && s[k - 1] != '\\'; k--) ;
    if (i - k >= sizeof(prog)) return NULL;
    memcpy(prog, s + k, i - k);
    prog[i - k] = '\0';
    if (strcmp(prog, "env") != 0 && prog[0] != '-') break;
    if (i >= n) return NULL;
  }
  for (k = strlen(prog); k > 0 && ((prog[k - 1] >= '0' && prog[k - 1] <= '9') || prog[k - 1] == '.'); k--)
    prog[k - 1] = '\0';	/* python3.11 -> python */
  for (k = 0; k < sizeof(progs) / sizeof(progs[0]); k++)
    if (strcmp(prog, progs[k].prog) == 0) return syntax_by_id(progs[k].id);
  return NULL;
}


/* the language of a file: by its name, else by its first line */
const Syntax *syntax_detect (const char *path, const Doc *d) {
  const Syntax *sx = path ? syntax_for(path) : NULL;
  if (sx == NULL && d && d->n > 0) sx = syntax_first_line(d->row[0].s, d->row[0].len);
  if (sx == NULL) sx = tm_detect(path, d);	/* one of VS Code's extensions knows it */
  return sx;
}


/* the languages there are, for Change Language Mode */
int syntax_count (void) {
  return (int)(sizeof(langs) / sizeof(langs[0]));
}


const Syntax *syntax_nth (int i) {
  return (i >= 0 && i < syntax_count()) ? &langs[i] : NULL;
}


const char *syntax_name (const Syntax *sx) {
  return sx ? sx->name : "Plain Text";
}


/* the language's comments (NULL: it has none of that kind) */
void syntax_comment (const Syntax *sx, const char **line, const char **open, const char **close) {
  *line = sx ? sx->line : NULL;
  *open = sx ? sx->open : NULL;
  *close = sx ? sx->close : NULL;
}

/* }================================================================== */


/*
** {==================================================================
** The scanner
** ===================================================================
*/

static int is_ident (int c, int dollar) {
  return c == '_' || c >= 0x80 || (c >= '0' && c <= '9') ||
         ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (dollar && c == '$');
}


static int starts (const char *s, size_t n, size_t i, const char *w) {
  size_t k = strlen(w);
  return i + k <= n && memcmp(s + i, w, k) == 0;
}


static int in_list (const char *const *list, const char *s, size_t n, int nocase) {
  if (list == NULL) return 0;
  for (; *list; list++)
    if (strlen(*list) == n && (nocase ? m_strnicmp(*list, s, n) : strncmp(*list, s, n)) == 0)
      return 1;
  return 0;
}


static void mark (unsigned char *tok, size_t from, size_t to, int t) {
  if (to > from) memset(tok + from, t, to - from);
}


/* a string from s[i] (its quote q) to its end on this line; where it ends */
static size_t string_end (const char *s, size_t n, size_t i, char q, unsigned char *tok, int *open) {
  size_t j = i + 1;
  tok[i] = T_STRING;
  *open = 1;
  while (j < n) {
    if (s[j] == '\\' && j + 1 < n && q != '`') {
      mark(tok, j, j + 2, T_ESCAPE);
      j += 2;
      continue;
    }
    tok[j] = T_STRING;
    if (s[j++] == q) {
      *open = 0;
      break;
    }
  }
  return j;
}


/* the end of a multi-line construct that started on an earlier line */
static size_t close_of (const Syntax *sx, int state, const char *s, size_t n, size_t i,
                        unsigned char *tok, int *still) {
  const char *end = state == ST_BLOCK ? sx->close : state == ST_BACKTICK ? "`" :
                    state == ST_TRIPLE2 ? "\"\"\"" : state == ST_TRIPLE1 ? "'''" : "]]";
  int t = state == ST_BLOCK ? T_COMMENT : T_STRING;
  size_t j = i, k = strlen(end);
  if (sx->flags & F_LUA && state == ST_BLOCK) end = "]]", k = 2;
  while (j < n && !(j + k <= n && memcmp(s + j, end, k) == 0)) j++;
  if (j < n) {
    mark(tok, i, j + k, t);
    *still = 0;
    return j + k;
  }
  mark(tok, i, n, t);
  *still = 1;
  return n;
}


/* HTML, XML: <tag attr="value">, &entities;, <!-- comments --> */
static int scan_markup (const Syntax *sx, const char *s, size_t n, int state, unsigned char *tok) {
  size_t i = 0;
  int value = 0;	/* after an attribute's '=': an unquoted value */
  if (state == ST_BLOCK) {
    int still;
    i = close_of(sx, ST_BLOCK, s, n, 0, tok, &still);
    if (still) return ST_BLOCK;
    state = ST_CODE;
  }
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (state == ST_TAG) {	/* in a tag: attributes and their values */
      if (c == '>' || (c == '/' && i + 1 < n && s[i + 1] == '>') || (c == '?' && i + 1 < n && s[i + 1] == '>')) {
        i += c == '>' ? 1 : 2;
        state = ST_CODE;
      }
      else if (c == '"' || c == '\'') {
        size_t e = i + 1;
        while (e < n && s[e] != (char)c) e++;
        e = e < n ? e + 1 : n;
        mark(tok, i, e, T_STRING);
        i = e;
        value = 0;
      }
      else if (is_ident(c, 0) || c == ':' || c == '@' || c == '#' || c == '.' || c == '-') {
        size_t e = i;
        while (e < n && (is_ident((unsigned char)s[e], 0) || strchr(":@#.-", s[e]))) e++;
        mark(tok, i, e, value ? T_STRING : T_VAR);
        i = e;
        value = 0;
      }
      else {
        if (c == '=') value = 1;
        else if (c != ' ' && c != '	') value = 0;
        i++;
      }
      continue;
    }
    if (c == '<' && i + 3 < n && memcmp(s + i, "<!--", 4) == 0) {
      int still;
      mark(tok, i, i + 4, T_COMMENT);
      i = close_of(sx, ST_BLOCK, s, n, i + 4, tok, &still);
      if (still) return ST_BLOCK;
      continue;
    }
    if (c == '<' && i + 1 < n && (is_ident((unsigned char)s[i + 1], 0) || s[i + 1] == '/' ||
                                  s[i + 1] == '!' || s[i + 1] == '?')) {
      size_t e = i + 1;
      if (e < n && (s[e] == '/' || s[e] == '!' || s[e] == '?')) e++;
      i = e;
      while (e < n && (is_ident((unsigned char)s[e], 0) || s[e] == '-' || s[e] == ':' || s[e] == '.')) e++;
      mark(tok, i, e, T_STORAGE);
      i = e;
      state = ST_TAG;
      continue;
    }
    if (c == '&') {	/* &amp; &#169; */
      size_t e = i + 1;
      while (e < n && e - i < 12 && (is_ident((unsigned char)s[e], 0) || s[e] == '#')) e++;
      if (e < n && s[e] == ';' && e > i + 1) {
        mark(tok, i, e + 1, T_ESCAPE);
        i = e + 1;
        continue;
      }
    }
    i++;
  }
  return state;
}


/* a diff: headers, + lines, - lines, like VS Code's diff language */
static int scan_diff (const char *s, size_t n, unsigned char *tok) {
  if (n == 0) return ST_CODE;
  if (starts(s, n, 0, "+++") || starts(s, n, 0, "---") || starts(s, n, 0, "@@") ||
      starts(s, n, 0, "diff ") || starts(s, n, 0, "index ") || starts(s, n, 0, "Index: ") ||
      starts(s, n, 0, "***"))
    mark(tok, 0, n, T_STORAGE);
  else if (s[0] == '+' || s[0] == '>') mark(tok, 0, n, T_NUMBER);
  else if (s[0] == '-' || s[0] == '<') mark(tok, 0, n, T_STRING);
  else if (s[0] == '!') mark(tok, 0, n, T_FUNC);
  return ST_CODE;
}


int syntax_scan (const Syntax *sx, const char *s, size_t n, int state, unsigned char *tok) {
  size_t i = 0;
  int fl, first = 1;	/* the first word of the line (shell: a command) */
  int words = 0;	/* the words so far (assembly: the first is the instruction) */
  mark(tok, 0, n, T_TEXT);
  if (sx == NULL) return ST_CODE;
  fl = sx->flags;
  if (fl & F_MARKUP) return scan_markup(sx, s, n, state, tok);
  if (fl & F_DIFF) return scan_diff(s, n, tok);
  if ((fl & F_INI) && state == ST_CODE) {	/* [section], key = value, ; comments */
    size_t j = 0, e;
    while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
    if (j < n && (s[j] == ';' || s[j] == '#')) {
      mark(tok, j, n, T_COMMENT);
      return ST_CODE;
    }
    if (j < n && s[j] == '[') {
      e = j;
      while (e < n && s[e] != ']') e++;
      mark(tok, j, e < n ? e + 1 : n, T_TYPE);
      i = e < n ? e + 1 : n;
    }
    else {
      for (e = j; e < n && s[e] != '=' && s[e] != ':' && s[e] != '"' && s[e] != '\''; e++) ;
      if (e < n && e > j && (s[e] == '=' || s[e] == ':')) {
        size_t k = e;
        while (k > j && (s[k - 1] == ' ' || s[k - 1] == '\t')) k--;
        if (starts(s, n, j, "export ")) j += 7;	/* .env: export KEY=value */
        mark(tok, j, k, T_VAR);
        i = e + 1;
      }
    }
    first = 0;
  }
  if (fl & F_MD) {	/* Markdown: headings, lists, quotes, `code` */
    size_t j = 0;
    while (j < n && s[j] == ' ') j++;
    if (state == ST_BACKTICK || starts(s, n, j, "```")) {
      mark(tok, 0, n, T_STRING);
      if (starts(s, n, j, "```")) return state == ST_BACKTICK ? ST_CODE : ST_BACKTICK;
      return state;
    }
    if (j < n && s[j] == '#') {
      mark(tok, 0, n, T_HEADING);
      return ST_CODE;
    }
    if (j < n && (s[j] == '-' || s[j] == '*' || s[j] == '>' || s[j] == '+')) tok[j] = T_KEYWORD;
    for (i = j; i < n; i++)
      if (s[i] == '`') {
        size_t e = i + 1;
        while (e < n && s[e] != '`') e++;
        mark(tok, i, e < n ? e + 1 : n, T_STRING);
        i = e;
      }
      else if (s[i] == '*' && i + 1 < n && s[i + 1] == '*') {
        size_t e = i + 2;
        while (e + 1 < n && !(s[e] == '*' && s[e + 1] == '*')) e++;
        if (e + 1 < n) {
          mark(tok, i, e + 2, T_STORAGE);
          i = e + 1;
        }
      }
    return ST_CODE;
  }
  if (state != ST_CODE) {
    int still;
    i = close_of(sx, state, s, n, 0, tok, &still);
    if (still) return state;
  }
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c == ' ' || c == '\t') {
      i++;
      continue;
    }
    /* comments: a block's opening first (#= in Julia, #[ in Nim) */
    if (sx->open && starts(s, n, i, sx->open)) {
      int still;
      size_t k = strlen(sx->open);
      mark(tok, i, i + k, T_COMMENT);
      i = close_of(sx, ST_BLOCK, s, n, i + k, tok, &still);
      if (still) return ST_BLOCK;
      continue;
    }
    if (sx->line && starts(s, n, i, sx->line) &&
        !((fl & F_SHELL) && c == '#' && i > 0 && s[i - 1] == '$')) {
      mark(tok, i, n, T_COMMENT);
      return ST_CODE;
    }
    if ((fl & F_ASM) && (c == '#' || (c == '/' && i + 1 < n && s[i + 1] == '/')) &&
        (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {	/* GAS comments */
      mark(tok, i, n, T_COMMENT);
      return ST_CODE;
    }
    if ((fl & F_TEX) && c == '\\' && i + 1 < n) {	/* \command, \\ */
      size_t e = i + 1;
      if (is_ident((unsigned char)s[e], 0) && s[e] != '_') {
        while (e < n && ((s[e] | 0x20) >= 'a' && (s[e] | 0x20) <= 'z')) e++;
      }
      else e++;
      mark(tok, i, e, T_STORAGE);
      i = e;
      continue;
    }
    if ((fl & F_NOCASE) && first && i + 3 <= n && m_strnicmp(s + i, "rem", 3) == 0 &&
        (i + 3 == n || s[i + 3] == ' ')) {	/* batch: rem is a comment */
      mark(tok, i, n, T_COMMENT);
      return ST_CODE;
    }
    if ((fl & F_LUA) && starts(s, n, i, "--[[")) {
      int still;
      mark(tok, i, i + 4, T_COMMENT);
      i = close_of(sx, ST_BLOCK, s, n, i + 4, tok, &still);
      if (still) return ST_BLOCK;
      continue;
    }
    /* the preprocessor: #include <x.h>, #define ... */
    if ((fl & F_PREPROC) && c == '#' && first) {
      size_t j = i + 1, w;
      while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
      w = j;
      while (j < n && is_ident((unsigned char)s[j], 0)) j++;
      mark(tok, i, j, T_KEYWORD);
      if (j - w == 7 && strncmp(s + w, "include", 7) == 0) {
        while (j < n && s[j] == ' ') j++;
        if (j < n && s[j] == '<') {
          size_t e = j;
          while (e < n && s[e] != '>') e++;
          mark(tok, j, e < n ? e + 1 : n, T_STRING);
          j = e < n ? e + 1 : n;
        }
      }
      else if (j - w == 6 && strncmp(s + w, "define", 6) == 0) {
        while (j < n && s[j] == ' ') j++;
        w = j;
        while (j < n && is_ident((unsigned char)s[j], 0)) j++;
        mark(tok, w, j, T_STORAGE);
      }
      first = 0;
      i = j;
      continue;
    }
    first = 0;
    /* strings */
    if ((fl & F_TRIPLE) && (starts(s, n, i, "\"\"\"") || starts(s, n, i, "'''"))) {
      int st = s[i] == '"' ? ST_TRIPLE2 : ST_TRIPLE1, still;
      mark(tok, i, i + 3, T_STRING);
      i = close_of(sx, st, s, n, i + 3, tok, &still);
      if (still) return st;
      continue;
    }
    if ((fl & F_LUA) && starts(s, n, i, "[[")) {
      int still;
      mark(tok, i, i + 2, T_STRING);
      i = close_of(sx, ST_LONG, s, n, i + 2, tok, &still);
      if (still) return ST_LONG;
      continue;
    }
    if (c == '`' && (fl & F_BACKTICK)) {
      int still;
      tok[i] = T_STRING;
      i = close_of(sx, ST_BACKTICK, s, n, i + 1, tok, &still);
      if (still) return ST_BACKTICK;
      continue;
    }
    if (sx->quotes && strchr(sx->quotes, c) && c != '`') {
      int open;
      size_t e;
      if ((fl & F_SHELL) && c == '\'' ) {	/* shell: '...' has no escapes */
        e = i + 1;
        while (e < n && s[e] != '\'') e++;
        e = e < n ? e + 1 : n;
        mark(tok, i, e, T_STRING);
      }
      else e = string_end(s, n, i, (char)c, tok, &open);
      if ((fl & F_JSON) && c == '"') {	/* a key: the string before a ':' */
        size_t k = e;
        while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
        if (k < n && s[k] == ':') mark(tok, i, e, T_VAR);
      }
      i = e;
      continue;
    }
    /* $var ${var} @var (PHP, Perl, Ruby, CMake) */
    if ((fl & F_SIGIL) && (c == '$' || c == '@') && i + 1 < n &&
        (is_ident((unsigned char)s[i + 1], 0) || (c == '$' && s[i + 1] == '{') ||
         (c == '@' && s[i + 1] == '@'))) {
      size_t e = i + 1;
      if (s[e] == '{') {
        while (e < n && s[e] != '}') e++;
        e = e < n ? e + 1 : n;
      }
      else {
        if (s[e] == '@') e++;
        while (e < n && is_ident((unsigned char)s[e], 0)) e++;
      }
      mark(tok, i, e, T_VAR);
      i = e;
      continue;
    }
    /* :symbols (Ruby, Elixir), not a::b nor x ? a : b */
    if ((fl & F_ATOM) && c == ':' && i + 1 < n && is_ident((unsigned char)s[i + 1], 0) &&
        !(s[i + 1] >= '0' && s[i + 1] <= '9') && (i == 0 || (!is_ident((unsigned char)s[i - 1], 0) &&
                                                             s[i - 1] != ':'))) {
      size_t e = i + 1;
      while (e < n && (is_ident((unsigned char)s[e], 0) || s[e] == '?' || s[e] == '!')) e++;
      mark(tok, i, e, T_CONST);
      i = e;
      continue;
    }
    /* shell variables: $x ${x} $(...) */
    if ((fl & F_SHELL) && c == '$' && i + 1 < n) {
      size_t e = i + 1;
      if (s[e] == '{' || s[e] == '(') {
        char cl = s[e] == '{' ? '}' : ')';
        while (e < n && s[e] != cl) e++;
        e = e < n ? e + 1 : n;
      }
      else while (e < n && (is_ident((unsigned char)s[e], 0) || strchr("?@#*!-", s[e]))) {
        e++;
        if (!is_ident((unsigned char)s[e - 1], 0)) break;
      }
      mark(tok, i, e, T_VAR);
      i = e;
      continue;
    }
    if ((fl & F_NOCASE) && c == '%') {	/* batch: %x% and %%x */
      size_t e = i + 1;
      while (e < n && s[e] != '%' && s[e] != ' ') e++;
      e = (e < n && s[e] == '%') ? e + 1 : e;
      mark(tok, i, e, T_VAR);
      i = e;
      continue;
    }
    /* numbers */
    if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9')) {
      size_t e = i;
      if (i > 0 && is_ident((unsigned char)s[i - 1], 0)) {
        i++;
        continue;
      }
      while (e < n && (is_ident((unsigned char)s[e], 0) || s[e] == '.' ||
                       ((s[e] == '+' || s[e] == '-') && (s[e - 1] == 'e' || s[e - 1] == 'E'))))
        e++;
      mark(tok, i, e, T_NUMBER);
      i = e;
      continue;
    }
    /* words */
    if (is_ident(c, (fl & F_DOLLAR) != 0) || ((fl & F_CSS) && c == '-')) {
      size_t e = i, k;
      int t = T_TEXT, nocase = (fl & (F_NOCASE | F_ICASE)) != 0;
      while (e < n && (is_ident((unsigned char)s[e], (fl & F_DOLLAR) != 0) ||
                       ((fl & F_CSS) && s[e] == '-')))
        e++;
      k = e;
      while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
      if ((fl & F_ASM) && k < n && s[k] == ':') t = T_FUNC;	/* a label */
      else if ((fl & F_ASM) && i > 0 && (s[i - 1] == '.' || s[i - 1] == '%')) {
        t = T_STORAGE;	/* .section, %define */
        tok[i - 1] = T_STORAGE;
      }
      else if (in_list(sx->control, s + i, e - i, nocase) && !((fl & F_LEAD) && words > 0)) t = T_KEYWORD;
      else if (in_list(sx->storage, s + i, e - i, nocase)) t = T_STORAGE;
      else if (in_list(sx->types, s + i, e - i, nocase)) t = T_TYPE;
      else if ((fl & F_ASM) && words == 0) t = T_KEYWORD;	/* the instruction */
      else if (fl & F_CSS) t = (k < n && s[k] == ':') ? T_VAR : T_TEXT;
      else if (fl & (F_SHELL | F_NOCASE | F_JSON)) t = T_TEXT;
      else if (k < n && s[k] == '(') t = T_FUNC;
      else if ((fl & F_PLAIN) && !(fl & F_CAPS)) t = T_TEXT;
      else if (fl & F_PLAIN) {
        size_t j;
        int lower = 0;
        for (j = i; j < e; j++)
          if (s[j] >= 'a' && s[j] <= 'z') lower = 1;
        t = (!lower && e - i > 1 && !(s[i] >= '0' && s[i] <= '9')) ? T_CONST : T_TEXT;
      }
      else {
        size_t j;
        int upper = 0, lower = 0;
        for (j = i; j < e; j++) {
          if (s[j] >= 'A' && s[j] <= 'Z') upper = 1;
          if (s[j] >= 'a' && s[j] <= 'z') lower = 1;
        }
        if ((fl & F_CAPS) && upper && !lower && e - i > 1) t = T_CONST;
        else if ((fl & F_UPPER) && s[i] >= 'A' && s[i] <= 'Z' && lower) t = T_TYPE;
        else if (e - i >= 2 && s[e - 2] == '_' && s[e - 1] == 't') t = T_TYPE;
        else t = T_VAR;
      }
      mark(tok, i, e, t);
      words++;
      i = e;
      continue;
    }
    i++;
  }
  return ST_CODE;
}

/* }================================================================== */


/*
** {==================================================================
** The states of a Doc's lines
** ===================================================================
*/

static unsigned char *g_tmp;	/* the tokens of lines only scanned for their state */
static size_t g_tmpcap;


void syntax_line (Doc *d, const Syntax *sx, size_t y, unsigned char *tok) {
  size_t k;
  const Row *r = &d->row[y];
  if (tm_line(d, sx, y, tok)) return;	/* VS Code's grammar colors it */
  if (d->hl_sx != (const void *)sx) {	/* another language: all again */
    d->hl_sx = sx;
    d->hl_n = 0;
  }
  if (d->hl_from < d->hl_n) d->hl_n = d->hl_from;	/* an edit: from there on again */
  d->hl_from = (size_t)-1;
  if (y + 2 > d->hl_cap) {
    d->hl_cap = d->hl_cap ? d->hl_cap : 1024;
    while (y + 2 > d->hl_cap) d->hl_cap *= 2;
    d->hl = (unsigned char *)xrealloc(d->hl, d->hl_cap);
  }
  if (d->hl_n == 0) {
    d->hl[0] = 0;	/* ST_CODE */
    d->hl_n = 1;
  }
  for (k = d->hl_n - 1; k < y; k++) {	/* the lines before, scanned only for their state */
    const Row *p = &d->row[k];
    if (p->len + 1 > g_tmpcap) {
      g_tmpcap = p->len + 256;
      g_tmp = (unsigned char *)xrealloc(g_tmp, g_tmpcap);
    }
    d->hl[k + 1] = (unsigned char)syntax_scan(sx, p->s, p->len, d->hl[k], g_tmp);
  }
  if (d->hl_n < y + 1) d->hl_n = y + 1;
  syntax_scan(sx, r->s, r->len, d->hl[y], tok);
}

/* }================================================================== */
