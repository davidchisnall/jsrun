/*
 * Copyright (c) 2015 David Chisnall
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * $FreeBSD$
 */

/**
 * ffigen is a simple program that walks declarations in a C file and
 * constructs duktap/C functions wrapping them.
 */

#include <clang-c/Index.h>
#include <cassert>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using std::cout;
using std::cerr;

struct Field
{
	std::string name;
	CXType type;
};

struct Struct
{
	std::vector<Field> fields;
};

struct Function
{
	std::string name;
	CXType type;
};

struct Enum
{
	std::vector<std::pair<std::string, int>> values;
};

static std::unordered_map<std::string, Struct> structs;
static std::unordered_map<std::string, Function> functions;
static std::unordered_map<std::string, Enum> enums;

/**
 * RAIICXString wraps a CXString and handles automatic deallocation.
 */
class RAIICXString
{
	CXString cxstr;
	public:
	RAIICXString(CXString string) : cxstr(string) {}
	const char *c_str() { return clang_getCString(cxstr); }
	std::string str() { std::string s(c_str()); return s; }
	operator const char *() { return c_str(); }
	operator std::string() { std::string s(c_str()); return s; }
	~RAIICXString() { clang_disposeString(cxstr); }
};

typedef std::function<CXChildVisitResult(CXCursor, CXCursor)> Visitor;

static CXChildVisitResult
visitChildrenLamdaTrampoline(CXCursor cursor,
                             CXCursor parent,
                             CXClientData client_data)
{
	return (*reinterpret_cast<Visitor*>(client_data))(cursor, parent);
}

static unsigned
visitChildren(CXCursor cursor, Visitor v)
{
	return clang_visitChildren(cursor, visitChildrenLamdaTrampoline,
			(CXClientData*)&v);
}

static void
collectStruct(CXCursor structDecl)
{
	if (structDecl.kind == CXCursor_UnionDecl)
	{
		return;
	}
	RAIICXString structname = clang_getCursorSpelling(structDecl);
	// If we've already parsed this struct, return early.
	if (structs.find(structname) != structs.end())
	{
		return;
	}
	Struct &s = structs[structname];
	visitChildren(structDecl,
		[&](CXCursor cursor, CXCursor parent)
		{
			CXCursorKind kind = clang_getCursorKind(cursor);
			RAIICXString str = clang_getCursorKindSpelling(kind);
			RAIICXString name = clang_getCursorSpelling(cursor);
			CXType type = clang_getCanonicalType(clang_getCursorType(cursor));
			if (type.kind == CXType_Unexposed)
			{
				visitChildren(cursor, 
					[&](CXCursor cursor, CXCursor parent)
					{
						type = clang_getCanonicalType(clang_getCursorType(cursor));
						return CXChildVisit_Break;
					});
			}
			RAIICXString type_name = clang_getTypeSpelling(type);
			if (type.kind == CXType_Record)
			{
				collectStruct(clang_getTypeDeclaration(type));
			}
			Field f = { name, type };
			s.fields.push_back(f);
			return CXChildVisit_Continue;
	});
}

static void
collectFunction(CXCursor functionDecl)
{
	RAIICXString name = clang_getCursorSpelling(functionDecl);
	CXType type = clang_getCanonicalType(clang_getCursorType(functionDecl));
	Function &f = functions[name];
	f.type = type;
}

static void
collectEnum(CXCursor enumDecl)
{
	RAIICXString name = clang_getCursorSpelling(enumDecl);
	Enum &e = enums[name];
	visitChildren(enumDecl,
		[&](CXCursor cursor, CXCursor parent)
		{
			RAIICXString name = clang_getCursorSpelling(cursor);
			int value = clang_getEnumConstantDeclValue(cursor);
			e.values.push_back(std::make_pair(name.str(), value));
			return CXChildVisit_Continue;
		});

}

static enum CXChildVisitResult
visitTranslationUnit (CXCursor cursor, CXCursor parent, CXClientData unused)
{
	CXCursorKind kind = clang_getCursorKind(cursor);
	// Skip anything that's deprecated
	if (clang_getCursorAvailability(cursor) != CXAvailability_Available)
	{
		return CXChildVisit_Continue;
	}
	// TODO: We currently only find 'bare' enum / struct declarations.
	// We ought to collect typedefs as well, as they're probably the ones that
	// programmers actually expect.
	switch (kind)
	{
		default:
			break;
		case CXCursor_StructDecl:
		{
			RAIICXString name = clang_getCursorSpelling(cursor);
			collectStruct(cursor);
			break;
		}
		case CXCursor_EnumDecl:
			collectEnum(cursor);
			break;
		case CXCursor_FunctionDecl:
			collectFunction(cursor);
			break;
	}
	return CXChildVisit_Continue;
}

template<class Stream>
void cast_to_js_fn(Stream &str, const std::string &name)
{
	str << "js_function_" << name << "_to_js";
}

template<class Stream>
void cast_from_js_fn(Stream &str, const std::string &name)
{
	str << "js_function_" << name << "_from_js";
}

bool
cast_to_js(CXType type, const std::string &cname, const std::string &jsname)
{
	bool ret = true;
	switch (type.kind)
	{
		default:
			return false;
		case CXType_Bool...CXType_LongLong:
			cout << "\tduk_push_int(ctx, "<< cname << ");\n";
			break;
		case CXType_Record:
		{
			auto decl = clang_getTypeDeclaration(type);
			if (decl.kind == CXCursor_UnionDecl)
			{
				// If it's a union then just construct a buffer and put the
				// data there.
				cout << "\t{\n\t\tvoid *buf = duk_push_fixed_buffer(ctx, "
				     << clang_Type_getSizeOf(type)
				     << ");\n\t\tmemcpy(buf, &("
				     << cname
				     << "), "
				     << clang_Type_getSizeOf(type)
				     << ");\n\t}";
				break;
			}
			// If it's a struct, then construct an object that corresponds to it.
			RAIICXString typeName = clang_getCursorSpelling(decl);
			cout << '\t';
			cast_to_js_fn(cout, typeName);
			cout << "(ctx, &(" << cname << "), 1);\n";
			cout << "\tduk_compact(ctx, -1);\n";
			break;
		}
		case CXType_ConstantArray:
		{
			CXType elementType = clang_getCanonicalType(clang_getElementType(type));
			long long len = clang_getNumElements(type);
			cout << "\t{\n\tduk_idx_t arr_idx = duk_push_array(ctx);\n";
			cout << "\tfor (int i=0 ; i<" << len << " ; i++)\n\t{\n";
			std::string elName = std::string("(") + cname + ")[i]";
			if (cast_to_js(elementType, elName, ""))
			{
				cout << "\tduk_put_prop_index(ctx, arr_idx, i);\n";
			}
			else
			{
				ret = false;
			}
			cout << "\t}\n\t}\n";
			break;
		}
		case CXType_Float...CXType_LongDouble:
			cout << "\tduk_push_number(ctx, (duk_double_t)"<< cname << ");\n";
			break;
		case CXType_Pointer:
		{
			RAIICXString ptrType = clang_getTypeSpelling(type);
			// FIXME: Special case C strings as JS strings
			cout << "\tduk_push_pointer(ctx, (void*)" << cname << ");\n";
			break;
		}
	}
	return ret;
}

/**
 * Helper function that emits code to that gets the top Duktape stack object as
 * `getType` if it is `ifType` and casts it to `cast` before storing it in
 * `ctype`.
 *
 * The `ifType` and `getType` parameters can differ, for example, if you wish
 * to check that the top value is a number and get it as an int or a double.
 */
static void
get_if(const char *ifType, const char *getType, const char *cast, const
		std::string cname)
{
	cout << "\tif (duk_is_" << ifType << "(ctx, -1))\n\t{"
	     << '\t' << cname << " = (" << cast << ')'
	     << "duk_get_" << getType << "(ctx, -1);\n"
	     << "\t}\n";
}
/**
 * Variant of `get_if` where `ifType` and `getType` are the same.
 */
static void
get_if(const char *type, const char *cast, const std::string cname)
{
	return get_if(type, type, cast, cname);
}

/**
 * Emit code to try to coerce the top item on the Duktape stack to `type` and
 * store it in `cname`.
 */
bool
cast_from_js(CXType type, const std::string &cname)
{
	bool ret = true;
	switch (type.kind)
	{
		default:
			return false;
		// If the target is an integer type, then try to fetch it as an int.
		case CXType_Bool...CXType_LongLong:
			get_if("number", "int", "long long", cname);
			break;
		// If we want a floating point value, try to get it as a float.
		case CXType_Float...CXType_LongDouble:
			get_if("number", "double", cname);
			break;
		// Record types include structs and unions. 
		case CXType_Record:
		{
			auto decl = clang_getTypeDeclaration(type);
			// If it's a union, ust get the raw data as a buffer.
			// FIXME: Once we have a TypedArray implementation, we'll want to
			// construct one of those.
			if (decl.kind == CXCursor_UnionDecl)
			{
					cout << "\tif (duk_is_buffer(ctx, -1))\n\t{\n"
					        "\tduk_size_t size;\n"
					        "\tvoid *buf = duk_get_buffer(ctx, -1, &size);\n"
					        "\tsize = size < "
					     << clang_Type_getSizeOf(type)
					     << " ? size : "
					     << clang_Type_getSizeOf(type)
					     << ";\n\tmemcpy("
					     << cname
					     << ", buf, size);\n\t}\n";
				break;
			}
			// For struct types, call the function that we've already emitted
			// (or are going to emit) that will perform the coercion.
			RAIICXString type = clang_getCursorSpelling(decl);
			cout << '\t';
			cast_from_js_fn(cout, type);
			cout << "(ctx, &(" << cname << "));\n";
			break;
		}
		case CXType_ConstantArray:
		{
			// For constant sized arrays, try to read each element from an
			// array parameter (or an object that looks a bit like an array).
			CXType elementType = clang_getCanonicalType(clang_getElementType(type));
			long long len = clang_getNumElements(type);
			cout << "\tfor (int i=0 ; i<" << len << " ; i++)\n\t{\n";
			cout << "\tduk_push_int(ctx, i);\n";
			cout << "\tif (duk_get_prop(ctx, -2)) {\n";
			std::string elName = std::string("(") + cname + ")[i]";
			cast_from_js(elementType, elName);
			cout << "\t}\n\tduk_pop(ctx);\n\t}";
			break;
		}
		case CXType_Pointer:
			// If it's a pointer, just store it as a pointer.  It's up to the
			// JS code to handle memory management correctly.
			get_if("pointer", "void*", cname);
			cout << "else if (duk_is_buffer(ctx, -1))\n\t{"
			        "\tduk_size_t size;\n\t\t"
			     << cname
			     << " = duk_get_buffer(ctx, -1, &size);\n\t}";
			break;
	}
	return ret;
}

/**
 * Returns true if the record type argument has some known fields.
 */
static bool
isCompleteRecordType(CXType type)
{
	assert(type.kind == CXType_Record);
	RAIICXString name =
		clang_getCursorSpelling(clang_getTypeDeclaration(type));
	auto i = structs.find(name);
	if (i == structs.end())
	{
		return false;
	}
	return !i->second.fields.empty();
}

void
emit_struct_wrappers()
{
	// First emit prototypes
	for (auto &kv : structs)
	{
		auto &s = kv.second;
		const std::string &sname = kv.first;
		cout << "inline static void ";
		cast_to_js_fn(cout, sname);
		cout << "(duk_context *ctx, struct "
		     << sname << " *obj, _Bool new_object);\n";

		cout << "inline static void ";
		cast_from_js_fn(cout, sname);
		cout << "(duk_context *ctx, struct " << sname << " *obj);\n";
	}
	for (auto &kv : structs)
	{
		auto &s = kv.second;
		const std::string &sname = kv.first;
		// If this is an empty / opaque struct, don't do anything with it...
		if (s.fields.empty())
		{
			continue;
		}
		// First emit the function for converting from a JS type to a C one.
		cout << "inline static void ";
		cast_to_js_fn(cout, sname);
		cout << "(duk_context *ctx, struct "
		     << sname << " *obj, _Bool new_object) {\n"
		        "\tif (new_object)\n\t{\n\t\tduk_push_object(ctx);\n\t}\n";
		for (auto &f : s.fields)
		{
			// Anonymous struct fields are assumed to be padding
			if (f.name == "")
			{
				continue;
			}
			std::string name = "obj->";
			name += f.name;
			if (cast_to_js(f.type, name, f.name))
			{
				cout << "\tduk_put_prop_string(ctx, -2, \"" << f.name << "\");\n";
			}
			else
			{
				RAIICXString kind = clang_getTypeKindSpelling(f.type.kind);
				cerr << "Warning: Unhandled field " << sname << '.'
					 << f.name << '\n';
				cerr << "Type: " << kind << '\n';
			}
		}
		cout << "\tduk_compact(ctx, -1);\n";
		cout << "}\n";
		// Now emit the function for converting JS to C
		cout << "inline static void ";
		cast_from_js_fn(cout, sname);
		cout << "(duk_context *ctx, struct " << sname << " *obj) {\n";
		cout << "\tbzero(obj, sizeof(*obj));\n";
		cout << "\tif (!duk_is_object(ctx, -1)) { return; }\n";
		for (auto &f : s.fields)
		{
			// Anonymous struct fields are assumed to be padding
			if (f.name == "")
			{
				continue;
			}
			std::string name = "obj->";
			name += f.name;
			cout << "\tduk_push_string(ctx, \"" << f.name << "\");\n";
			cout << "\tif (duk_get_prop(ctx, -2)) {\n";
			// No error reporting here, because we assume that we'll have
			// already handled errors.
			cast_from_js(f.type, name);
			cout << "\t}\n\tduk_pop(ctx);\n";
		}
		cout << "}\n";
	}
}

void
emit_function_wrappers()
{
	// We'll try to emit a function wrapping each C function.  If we're not
	// sure that we've managed, then we'll emit a warning and continue.  We'll
	// then put all of the ones that we successfully handled in a function
	// list and register them with the JS context.
	std::vector<std::tuple<const std::string, const std::string, int>> fns;
	for (auto kv : functions)
	{
		CXType fnType = kv.second.type;
		const std::string &name = kv.first;
		// We don't have a way of constructing variadic calls at run time, so
		// we can't bridge them automatically without linking in libffi or
		// similar.  Skip them for now.
		if (clang_isFunctionTypeVariadic(fnType))
		{
			cerr << "Warning: " << name << " is variadic.  Skipping...\n";
			continue;
		}
		CXType retTy = clang_getResultType(fnType);
		if ((retTy.kind == CXType_Pointer) &&
		    (clang_getPointeeType(retTy).kind == CXType_FunctionProto))
		{
			cerr << "Warning: Can't yet handle function pointer returns for "
			     << name << ".\n";
			continue;
		}
		const std::string cname = std::string("js_func_") + name + "_wrapped";
		bool success = true;
		RAIICXString type = clang_getTypeSpelling(fnType);
		int args = clang_getNumArgTypes(fnType);
		cout << "static int js_func_" << name
		     << "_wrapped(duk_context *ctx)\n{\n";
		// If we have the wrong number of arguments, then abort
		cout << "\tif (duk_get_top(ctx) != " << args << ")\n\t{";
		cout << "\treturn DUK_RET_TYPE_ERROR;\n\t}\n";
		std::unordered_set<int> writeback;
		for (int i=0 ; i<args ; i++)
		{
			bool special = false;
			std::stringstream ss;
			ss << "arg" << i;
			std::string argName = ss.str();
			CXType argType = clang_getArgType(fnType, i);
			cout << "\tduk_dup(ctx, -" << (args-i) << ");\n";
			RAIICXString typeName = clang_getTypeSpelling(argType);
			if (argType.kind == CXType_Pointer)
			{
				CXType pointee =
					clang_getCanonicalType(clang_getPointeeType(argType));
				if (pointee.kind == CXType_Char_S ||
				    pointee.kind == CXType_Void)
				{
					special = true;
					cout << typeName << " arg" << i << ";\n";
					get_if("string", "char*", argName);
					cout << "\telse\n";
					cast_from_js(argType, argName);
				}
				else if (pointee.kind == CXType_FunctionProto)
				{
					special = true;
					success = false;
					cerr << "Warning: Can't yet handle function pointer args\n";
				}
				else  if ((pointee.kind == CXType_Record) &&
				          isCompleteRecordType(pointee))
				{
					special = true;
					writeback.insert(i);
					RAIICXString pointeeName = clang_getTypeSpelling(pointee);
					RAIICXString pointeeKind = clang_getTypeKindSpelling(pointee.kind);
					cout << "int writeback_" << argName << " = 0;\n";
					cout << typeName << ' ' << argName << ";\n";
					std::string bufName = argName + "_buf";
					cout << pointeeName << ' ' << bufName << ";\n";
					get_if("pointer", "void*", argName);
					cout << "\telse\n\t{\n";
					cast_from_js(pointee, bufName);
					cout << argName << " = &" << bufName << ";\n";
					cout << "writeback_" << argName << " = 1;\n";
					cout << "\t}";
				}
			}
			if (!special)
			{
				cout << typeName << " arg" << i << ";\n";
				if (!cast_from_js(argType, argName))
				{
					success = false;
					break;
				}
			}
			cout << "\tduk_pop(ctx);\n";
			// FIXME: Do something more sensible with pointer types
			// FIXME: Some args may be in-out so flag that we need to copy them
			// back...
		}
		if (success)
		{
			RAIICXString typeName = clang_getTypeSpelling(retTy);
			if (retTy.kind != CXType_Void)
			{
				cout << typeName << " ret = ";
			}
			cout << name << '(';
			for (int i=0 ; i<args ; i++)
			{
				// If we've created something that's a desugared type, emit a
				// cast to the typedef type to silence compiler warnings.
				RAIICXString argTy =
					clang_getTypeSpelling(clang_getArgType(fnType, i));
				cout << " arg" << i;
				if (i<args-1)
				{
					cout << ", ";
				}
			}
			cout << ");";
			// After the call, we iterate over all of the values that we should
			// be writing back.
			for (auto i : writeback)
			{
				CXType argType = clang_getArgType(fnType, i);
				CXType type =
					clang_getCanonicalType(clang_getPointeeType(argType));
				auto decl = clang_getTypeDeclaration(type);
				std::stringstream ss;
				ss << "arg" << i;
				std::string argName = ss.str();
				RAIICXString typeName = clang_getCursorSpelling(decl);
				cout << "\tif (writeback_" << argName << ")\n\t{\n";
				cout << "\tduk_dup(ctx, -" << (args-i) << ");\n";
				cout << '\t';
				cast_to_js_fn(cout, typeName);
				cout << "(ctx, &(" << argName << "_buf), 0);\n";
				cout << "\tduk_pop(ctx);\n\t}";
			}
			success &= cast_to_js(retTy, "ret", "");
		}
		if (retTy.kind == CXType_Void)
		{
			cout << "\treturn 0;\n";
		}
		else
		{
			cout << "\treturn 1;\n";
		}
		cout << "}\n";
		if (success)
		{
			fns.push_back(std::make_tuple(name, cname, args));
		}
	}
	// Emit the function list
	cout << "static const duk_function_list_entry js_funcs[] = {\n";
	for (auto &entry : fns)
	{
		cout << "\t{ \"" << std::get<0>(entry) << "\", " << std::get<1>(entry)
		     << ", " << std::get<2>(entry) << "},\n";
	}
	cout << "\t{ 0, 0, 0 }\n";
	cout << "};\n";
}

void
emit_enum_wrappers()
{
	cout << "duk_ret_t dukopen_module(duk_context *ctx)\n{\n"
	     << "\tduk_push_object(ctx);\n"
	     << "\tduk_put_function_list(ctx, -1, js_funcs);\n";
	for (auto &kv : enums)
	{
		const std::string &name = kv.first;
		Enum &vals = kv.second;
		if (name != std::string())
		{
			cout << "\tduk_push_object(ctx);\n";
		}
		for (auto &v : vals.values)
		{
			cout << "\tduk_push_int(ctx, " << v.second << ");\n"
			     << "\tduk_put_prop_string(ctx, -2, \"" << v.first << "\");\n";
		}
		if (name != std::string())
		{
			cout << "\tduk_put_prop_string(ctx, -2, \"" << name << "\");\n";
		}
	}
	cout << "\treturn 1;\n}\n";
}

int
main(int argc, char **argv)
{
	if (argc < 2)
	{
		cerr << "Usage: " << argv[0] << "{header} [compiler flags]\n";
		return EXIT_FAILURE;
	}
	// Construct the libclang context and try to parse the file.
	CXIndex idx = clang_createIndex(1, 1);
	CXTranslationUnit translationUnit =
		clang_createTranslationUnitFromSourceFile(idx, argv[1], argc-2, argv+2,
				0, nullptr);
	if (!translationUnit)
	{
		cerr << "Unable to parse file\n";
		return EXIT_FAILURE;
	}
	clang_visitChildren(clang_getTranslationUnitCursor(translationUnit),
			visitTranslationUnit, 0);
	// Emit all of the wrapers
	emit_struct_wrappers();
	emit_function_wrappers();
	emit_enum_wrappers();
	// Clean up (don't bother for non-debug builds, exit is our garbage
	// collector!)
#ifdef NDEBUG
	clang_disposeTranslationUnit(translationUnit);
	clang_disposeIndex(idx);
#endif
}

