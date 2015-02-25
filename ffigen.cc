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

namespace {

/**
 * Type for metadata about structs.  We collect the names and types of fields
 * for each struct.
 */
typedef std::vector<std::pair<std::string, CXType>> Struct;
/**
 * Type for metadata about enums.  We collect the name and value for each enum
 * value.
 */
typedef std::vector<std::pair<std::string, int>> Enum;
/**
 * Type for visitors passed to `visitChildren`.
 */
typedef std::function<CXChildVisitResult(CXCursor, CXCursor)> Visitor;

/**
 * Global collection of all of the structs that we've found.
 */
std::unordered_map<std::string, Struct> structs;
/**
 * Global collection of all of the function declarations that we've found.
 */
std::unordered_map<std::string, CXType> functions;
/**
 * Global collection of all of the enumerations that we've found.
 */
std::unordered_map<std::string, Enum> enums;

/**
 * RAIICXString wraps a CXString and handles automatic deallocation.
 */
class RAIICXString
{
	/**
	 * The string that this wraps.
	 */
	CXString cxstr;
	public:
	/**
	 * Construct from a libclang string.
	 */
	RAIICXString(CXString string) : cxstr(string) {}
	/**
	 * Extract the C string from this string when cast to a C string.
	 */
	operator const char *() { return clang_getCString(cxstr); }
	/**
	 * Extract the C string from this string and convert it to a `std::string`.
	 */
	std::string str() { return std::string(clang_getCString(cxstr)); }
	/**
	 * Allow casts to a `std::string`.
	 */
	operator std::string() { return str(); }
	/**
	 * Destroy the underlying string.
	 */
	~RAIICXString() { clang_disposeString(cxstr); }
};

/**
 * Trampoline used by visitChildren to call a `std::function` instead of a C
 * function.
 */
CXChildVisitResult
visitChildrenTrampoline(CXCursor cursor,
                        CXCursor parent,
                        CXClientData client_data)
{
	return (*reinterpret_cast<Visitor*>(client_data))(cursor, parent);
}

/**
 * `clang_visitChildren` wrapper that takes a `std::function`.
 */
unsigned
visitChildren(CXCursor cursor, Visitor v)
{
	return clang_visitChildren(cursor, visitChildrenTrampoline,
			(CXClientData*)&v);
}

/**
 * Collect struct definitions.
 */
void
collectStruct(CXCursor structDecl)
{
	// Skip unions - we don't explicitly box them as objects, we just wrap them
	// in a buffer.
	if (structDecl.kind == CXCursor_UnionDecl)
	{
		return;
	}
	RAIICXString structname = clang_getCursorSpelling(structDecl);
	if (structname.str() == "")
	{
		return;
	}
	// If we've already parsed this struct, return early.
	if (structs.find(structname) != structs.end())
	{
		return;
	}
	Struct &s = structs[structname];
	// Once we've found a struct, recursively visit the fields and add them.
	visitChildren(structDecl,
		[&](CXCursor cursor, CXCursor parent)
		{
			CXCursorKind kind = clang_getCursorKind(cursor);
			RAIICXString str = clang_getCursorKindSpelling(kind);
			RAIICXString name = clang_getCursorSpelling(cursor);
			CXType type = clang_getCanonicalType(clang_getCursorType(cursor));
			RAIICXString type_name = clang_getTypeSpelling(type);
			// FIXME: We currently don't handle anonymous structs inside other
			// structs, which we should...
			if (type.kind == CXType_Record)
			{
				collectStruct(clang_getTypeDeclaration(type));
			}
			s.push_back(std::make_pair(name.str(), type));
			return CXChildVisit_Continue;
	});
}

/**
 * Collect function declarations.
 */
void
collectFunction(CXCursor functionDecl)
{
	RAIICXString name = clang_getCursorSpelling(functionDecl);
	CXType type = clang_getCanonicalType(clang_getCursorType(functionDecl));
	functions[name] = type;
}

/**
 * Collect enum declarations.
 */
void
collectEnum(CXCursor enumDecl)
{
	RAIICXString name = clang_getCursorSpelling(enumDecl);
	Enum &e = enums[name];
	// Recursively visit the children of the enum.
	visitChildren(enumDecl,
		[&](CXCursor cursor, CXCursor parent)
		{
			RAIICXString name = clang_getCursorSpelling(cursor);
			int value = clang_getEnumConstantDeclValue(cursor);
			e.push_back(std::make_pair(name.str(), value));
			return CXChildVisit_Continue;
		});

}

/**
 * Top-level visit function.  Iterate over all top-level declarations and
 * collect information about them.
 */
enum CXChildVisitResult
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

/**
 * Helper that emits the name of the function used to convert from a C
 * structure to JavaScript.
 */
template<class Stream>
void cast_to_js_fn(Stream &str, const std::string &name)
{
	str << "js_function_" << name << "_to_js";
}

/**
 * Helper that emits the name of the function used to convert to a C structure
 * from JavaScript.
 */
template<class Stream>
void cast_from_js_fn(Stream &str, const std::string &name)
{
	str << "js_function_" << name << "_from_js";
}

/**
 * Emit code to convert the variable named by `cname` to JavaScript and store
 * it on the top of the Duktape stack.  The type of the C variable is specified
 * by `type`.
 */
bool
cast_to_js(CXType type, const std::string &cname)
{
	bool ret = true;
	switch (type.kind)
	{
		default:
		{
			RAIICXString typeName = clang_getTypeSpelling(type);
			cerr << "Warning: Unable to handle type " << typeName << '\n';
			ret = false;
			break;
		}
		case CXType_Void:
			break;
		case CXType_Bool:
			cout << "\tduk_push_boolean(ctx, "<< cname << ");\n";
			break;
		// Unsigned types up to int, push as int
		case CXType_Char_U:
		case CXType_UChar:
		case CXType_UShort:
		case CXType_UInt:
			cout << "\tduk_push_uint(ctx, "<< cname << ");\n";
			break;
		// Signed types up to int, push as int
		case CXType_Char_S:
		case CXType_Char16:
		case CXType_Char32:
		case CXType_SChar:
		case CXType_WChar:
		case CXType_Short:
		case CXType_Int:
			cout << "\tduk_push_int(ctx, "<< cname << ");\n";
			break;
		// Types bigger than an int, push as a double
		case CXType_Long:
		case CXType_LongLong:
		case CXType_ULong:
		case CXType_ULongLong:
		case CXType_Float...CXType_LongDouble:
			cout << "\tduk_push_number(ctx, (duk_double_t)"<< cname << ");\n";
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
			// If it's a struct, then construct an object that corresponds to
			// it.
			RAIICXString typeName = clang_getCursorSpelling(decl);
			cout << '\t';
			cast_to_js_fn(cout, typeName);
			cout << "(ctx, &(" << cname << "), 1);\n";
			cout << "\tduk_compact(ctx, -1);\n";
			break;
		}
		case CXType_ConstantArray:
		{
			// For constant-sized arrays, construct an array that has the same
			// elements.
			CXType elementType = clang_getCanonicalType(clang_getElementType(type));
			long long len = clang_getNumElements(type);
			cout << "\t{\n\tduk_idx_t arr_idx = duk_push_array(ctx);\n";
			cout << "\tfor (int i=0 ; i<" << len << " ; i++)\n\t{\n";
			std::string elName = std::string("(") + cname + ")[i]";
			if (cast_to_js(elementType, elName))
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
void
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
void
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
		{
			RAIICXString typeName = clang_getTypeSpelling(type);
			cerr << "Warning: Unable to handle type " << typeName << '\n';
			ret = false;
			break;
		}
		case CXType_Void:
			break;
		case CXType_Bool:
		{
			RAIICXString typeName = clang_getTypeSpelling(type);
			get_if("boolean", "boolean", typeName, cname);
			break;
		}
		// Unsigned types up to int, fetch as int
		case CXType_Char_U:
		case CXType_UChar:
		case CXType_UShort:
		case CXType_UInt:
		{
			RAIICXString typeName = clang_getTypeSpelling(type);
			get_if("number", "uint", typeName, cname);
			break;
		}
		// Signed types up to int, get as int
		case CXType_Char_S:
		case CXType_Char16:
		case CXType_Char32:
		case CXType_SChar:
		case CXType_WChar:
		case CXType_Short:
		case CXType_Int:
		{
			RAIICXString typeName = clang_getTypeSpelling(type);
			get_if("number", "int", typeName, cname);
			break;
		}
		// Types bigger than an int, get as a double
		case CXType_Long:
		case CXType_LongLong:
		case CXType_ULong:
		case CXType_ULongLong:
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
				     << ";\n\tmemcpy(&("
				     << cname
				     << "), buf, size);\n\t}\n";
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
bool
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
	return !i->second.empty();
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
		if (s.empty())
		{
			continue;
		}
		// First emit the function for converting from a JS type to a C one.
		cout << "inline static void ";
		cast_to_js_fn(cout, sname);
		cout << "(duk_context *ctx, struct "
		     << sname << " *obj, _Bool new_object) {\n"
		        "\tif (new_object)\n\t{\n\t\tduk_push_object(ctx);\n\t}\n";
		for (auto &f : s)
		{
			std::string &fname = f.first;
			CXType &ftype = f.second;
			// Anonymous struct fields are assumed to be padding
			if (fname == "")
			{
				continue;
			}
			std::string name = "obj->";
			name += fname;
			if (cast_to_js(ftype, name))
			{
				cout << "\tduk_put_prop_string(ctx, -2, \"" << fname << "\");\n";
			}
			else
			{
				RAIICXString kind = clang_getTypeKindSpelling(ftype.kind);
				cerr << "Warning: Unhandled field " << sname << '.'
					 << fname << '\n';
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
		for (auto &f : s)
		{
			std::string &fname = f.first;
			CXType &ftype = f.second;
			// Anonymous struct fields are assumed to be padding
			if (fname == "")
			{
				continue;
			}
			std::string name = "obj->";
			name += fname;
			cout << "\tduk_push_string(ctx, \"" << fname << "\");\n";
			cout << "\tif (duk_get_prop(ctx, -2)) {\n";
			// No error reporting here, because we assume that we'll have
			// already handled errors.
			cast_from_js(ftype, name);
			cout << "\t}\n\tduk_pop(ctx);\n";
		}
		cout << "}\n";
	}
}

template<class T> bool
emit_function_argument(CXType fnType, int args, int i, T &writeback)
{
	bool success = true;
	bool special = false;
	std::stringstream ss;
	ss << "arg" << i;
	std::string argName = ss.str();
	CXType argType = clang_getArgType(fnType, i);
	cout << "\tduk_dup(ctx, -" << (args-i) << ");\n";
	RAIICXString typeName = clang_getTypeSpelling(argType);
	// FIXME: We should handle block args by emitting a block that wraps a
	// JavaScript function.
	if (argType.kind == CXType_BlockPointer)
	{
		special = true;
		success = false;
		cerr << "Warning: Can't yet handle block pointer args\n";
	}
	else if (argType.kind == CXType_Pointer)
	{
		CXType pointee = clang_getPointeeType(argType);
		RAIICXString str = clang_getTypeSpelling(pointee);
		bool isConst = clang_isConstQualifiedType(pointee);
		pointee = clang_getCanonicalType(pointee);
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
			if (!isConst)
			{
				writeback.insert(i);
				cout << "int writeback_" << argName << " = 0;\n";
			}
			RAIICXString pointeeName = clang_getTypeSpelling(pointee);
			RAIICXString pointeeKind = clang_getTypeKindSpelling(pointee.kind);
			cout << typeName << ' ' << argName << ";\n";
			std::string bufName = argName + "_buf";
			cout << pointeeName << ' ' << bufName << ";\n";
			get_if("pointer", "void*", argName);
			cout << "\telse\n\t{\n";
			cast_from_js(pointee, bufName);
			cout << argName << " = &" << bufName << ";\n";
			if (!isConst)
			{
				cout << "writeback_" << argName << " = 1;\n";
			}
			cout << "\t}";
		}
	}
	if (!special)
	{
		cout << typeName << " arg" << i << ";\n";
		if (!cast_from_js(argType, argName))
		{
			success = false;
		}
	}
	cout << "\tduk_pop(ctx);\n";
	return success;
}

void
emit_function_call(CXType fnType, CXType retTy, int args, const std::string &name)
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
}

template<class T> void
emit_function_arg_writeback(T writeback, CXType fnType, int args)
{
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
		CXType fnType = kv.second;
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
			success &= emit_function_argument(fnType, args, i, writeback);
		}
		if (success)
		{
			emit_function_call(fnType, retTy, args, name);
			emit_function_arg_writeback(writeback, fnType, args);
			if (retTy.kind == CXType_Pointer)
			{
				CXType pointee = clang_getPointeeType(retTy);
				pointee = clang_getCanonicalType(pointee);
				if ((pointee.kind == CXType_Record) &&
				    isCompleteRecordType(pointee))
				{
					cout << "\tif (ret != 0)\n\t{\n\t";
					success &= cast_to_js(pointee, "(*ret)");
					cout << "} else {\n\t\tduk_push_null(ctx);\n\t}";
				}
				else
				{
					success &= cast_to_js(retTy, "ret");
				}
			}
			else
			{
				// We don't need to bracket this in a check for void, because
				// cast_to_js will not emit anything when a void value is passed.
				success &= cast_to_js(retTy, "ret");
			}
		}
		// Return undefined (0 return values) for a void function, one value
		// for anything else.
		if (retTy.kind == CXType_Void)
		{
			cout << "\treturn 0;\n";
		}
		else
		{
			cout << "\treturn 1;\n";
		}
		cout << "}\n";
		// If we've managed to successfully emit this function wrapper, then
		// add it to the list to emit.  If anything went wrong, it's static and
		// unused, so the compiler will discard it when compiling the generated
		// wrappers.
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
	// Add the null terminator.
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
		for (auto &v : vals)
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

} // anonymous namespace

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

