/*
    This file is part of Corrade.

    Copyright © 2007, 2008, 2009, 2010, 2011, 2012, 2013, 2014
              Vladimír Vondruš <mosra@centrum.cz>

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include "Resource.h"

#include <algorithm> /* std::max(), needed by MSVC */
#include <iomanip>
#include <sstream>
#include <tuple>
#include <vector>

#include "Corrade/Containers/Array.h"
#include "Corrade/Utility/Assert.h"
#include "Corrade/Utility/Configuration.h"
#include "Corrade/Utility/Debug.h"
#include "Corrade/Utility/Directory.h"

namespace Corrade { namespace Utility {

struct Resource::OverrideData {
    const Configuration conf;
    std::map<std::string, Containers::Array<unsigned char>> data;

    explicit OverrideData(const std::string& filename): conf(filename) {}
};

auto Resource::resources() -> std::map<std::string, GroupData>& {
    static std::map<std::string, GroupData> resources;
    return resources;
}

void Resource::registerData(const char* group, unsigned int count, const unsigned char* positions, const unsigned char* filenames, const unsigned char* data) {
    /* Already registered */
    /** @todo Fix and assert that this doesn't happen */
    if(resources().find(group) != resources().end()) return;

    #ifndef CORRADE_GCC47_COMPATIBILITY
    const auto groupData = resources().emplace(group, GroupData()).first;
    #else
    const auto groupData = resources().insert(std::make_pair(group, GroupData())).first;
    #endif

    /* Cast to type which can be eaten by std::string constructor */
    const char* _positions = reinterpret_cast<const char*>(positions);
    const char* _filenames = reinterpret_cast<const char*>(filenames);

    const unsigned int size = sizeof(unsigned int);
    unsigned int oldFilenamePosition = 0, oldDataPosition = 0;

    /* Every 2*sizeof(unsigned int) is one data */
    for(unsigned int i = 0; i != count*2*size; i += 2*size) {
        unsigned int filenamePosition = *reinterpret_cast<const unsigned int*>(_positions+i);
        unsigned int dataPosition = *reinterpret_cast<const unsigned int*>(_positions+i+size);

        Containers::ArrayReference<const unsigned char> res(data+oldDataPosition, dataPosition-oldDataPosition);

        #ifndef CORRADE_GCC47_COMPATIBILITY
        groupData->second.resources.emplace(std::string(_filenames+oldFilenamePosition, filenamePosition-oldFilenamePosition), res);
        #else
        groupData->second.resources.insert(std::make_pair(std::string(_filenames+oldFilenamePosition, filenamePosition-oldFilenamePosition), res));
        #endif

        oldFilenamePosition = filenamePosition;
        oldDataPosition = dataPosition;
    }
}

void Resource::unregisterData(const char* group) {
    /** @todo test this */
    auto it = resources().find(group);
    CORRADE_ASSERT(it != resources().end(),
        "Utility::Resource: resource group" << group << "is not registered", );

    resources().erase(it);
}

std::string Resource::compileFrom(const std::string& name, const std::string& configurationFile) {
    /* Resource file existence */
    if(!Directory::fileExists(configurationFile)) {
        Error() << "    Error: file" << configurationFile << "does not exist";
        return {};
    }

    const std::string path = Directory::path(configurationFile);
    const Configuration conf(configurationFile, Configuration::Flag::ReadOnly);

    /* Group name */
    if(!conf.hasValue("group")) {
        Error() << "    Error: group name is not specified";
        return {};
    }
    const std::string group = conf.value("group");

    /* Load all files */
    std::vector<const ConfigurationGroup*> files = conf.groups("file");
    std::vector<std::pair<std::string, std::string>> fileData;
    fileData.reserve(files.size());
    for(auto it = files.begin(); it != files.end(); ++it) {
        const ConfigurationGroup* const file = *it;

        Debug() << "Reading file" << fileData.size()+1 << "of" << files.size() << "in group" << '\'' + group + '\'';

        const std::string filename = file->value("filename");
        const std::string alias = file->hasValue("alias") ? file->value("alias") : filename;
        if(filename.empty() || alias.empty()) {
            Error() << "    Error: filename or alias is empty";
            return {};
        }

        Debug() << "   " << filename;
        if(alias != filename) Debug() << " ->" << alias;

        bool success;
        Containers::Array<unsigned char> contents;
        std::tie(success, contents) = fileContents(Directory::join(path, filename));
        if(!success) return {};
        fileData.emplace_back(std::move(alias), std::string(reinterpret_cast<char*>(contents.begin()), contents.size()));
    }

    return compile(name, group, fileData);
}

std::string Resource::compile(const std::string& name, const std::string& group, const std::vector<std::pair<std::string, std::string>>& files) {
    /* Special case for empty file list */
    if(files.empty()) {
        return "/* Compiled resource file. DO NOT EDIT! */\n\n"
            "#include \"Corrade/compatibility.h\"\n"
            "#include \"Corrade/Utility/Macros.h\"\n"
            "#include \"Corrade/Utility/Resource.h\"\n\n"
            "int resourceInitializer_" + name + "();\n"
            "int resourceInitializer_" + name + "() {\n"
            "    Corrade::Utility::Resource::registerData(\"" + group + "\", 0, nullptr, nullptr, nullptr);\n"
            "    return 1;\n"
            "} CORRADE_AUTOMATIC_INITIALIZER(resourceInitializer_" + name + ")\n\n"
            "int resourceFinalizer_" + name + "();\n"
            "int resourceFinalizer_" + name + "() {\n"
            "    Corrade::Utility::Resource::unregisterData(\"" + group + "\");\n"
            "    return 1;\n"
            "} CORRADE_AUTOMATIC_FINALIZER(resourceFinalizer_" + name + ")\n";
    }

    std::string positions, filenames, data;
    unsigned int filenamesLen = 0, dataLen = 0;

    /* Convert data to hexacodes */
    for(auto it = files.cbegin(); it != files.cend(); ++it) {
        filenamesLen += it->first.size();
        dataLen += it->second.size();

        if(it != files.begin()) {
            filenames += '\n';
            data += '\n';
        }

        positions += hexcode(numberToString(filenamesLen));
        positions += hexcode(numberToString(dataLen));

        filenames += comment(it->first);
        filenames += hexcode(it->first);

        data += comment(it->first);
        data += hexcode(it->second);
    }

    /* Remove last comma from positions and filenames array */
    positions.resize(positions.size()-1);
    filenames.resize(filenames.size()-1);

    /* Remove last comma from data array only if the last file is not empty */
    if(!files.back().second.empty())
        data.resize(data.size()-1);

    #if defined(CORRADE_TARGET_NACL_NEWLIB) || defined(__MINGW32__)
    std::ostringstream converter;
    converter << files.size();
    #endif

    /* Return C++ file. The functions have forward declarations to avoid warning
       about functions which don't have corresponding declarations (enabled by
       -Wmissing-declarations in GCC). If we don't have any data, we don't
       create the resourceData array, as zero-length arrays are not allowed.
       The corradeCompatibility.h must be included even if we don't need it in
       master branch, because the user might want to compile resource file for
       Corrade in compatibility branch with Corrade in master branch (i.e. x86
       NaCl). */
    return "/* Compiled resource file. DO NOT EDIT! */\n\n"
        "#include \"Corrade/compatibility.h\"\n"
        "#include \"Corrade/Utility/Macros.h\"\n"
        "#include \"Corrade/Utility/Resource.h\"\n\n"
        "static const unsigned char resourcePositions[] = {" +
        positions + "\n};\n\n"
        "static const unsigned char resourceFilenames[] = {" +
        filenames + "\n};\n\n" +
        (dataLen ? "" : "// ") + "static const unsigned char resourceData[] = {" +
        data + '\n' + (dataLen ? "" : "// ") + "};\n\n" +
        "int resourceInitializer_" + name + "();\n"
        "int resourceInitializer_" + name + "() {\n"
        "    Corrade::Utility::Resource::registerData(\"" + group + "\", " +
            /* This shouldn't be ambiguous. But is. */
            #if !defined(CORRADE_TARGET_NACL_NEWLIB) && !defined(__MINGW32__)
            #ifndef CORRADE_GCC44_COMPATIBILITY
            std::to_string(files.size()) +
            #else
            std::to_string(static_cast<unsigned long long int>(files.size())) +
            #endif
            #else
            converter.str() +
            #endif
        ", resourcePositions, resourceFilenames, " + (dataLen ? "resourceData" : "nullptr") + ");\n"
        "    return 1;\n"
        "} CORRADE_AUTOMATIC_INITIALIZER(resourceInitializer_" + name + ")\n\n"
        "int resourceFinalizer_" + name + "();\n"
        "int resourceFinalizer_" + name + "() {\n"
        "    Corrade::Utility::Resource::unregisterData(\"" + group + "\");\n"
        "    return 1;\n"
        "} CORRADE_AUTOMATIC_FINALIZER(resourceFinalizer_" + name + ")\n";
}

void Resource::overrideGroup(const std::string& group, const std::string& configurationFile) {
    auto it = resources().find(group);
    CORRADE_ASSERT(it != resources().end(),
        "Utility::Resource::overrideGroup(): group" << '\'' + group + '\'' << "was not found", );
    it->second.overrideGroup = configurationFile;
}

Resource::Resource(const std::string& group): _overrideGroup(nullptr) {
    _group = resources().find(group);
    CORRADE_ASSERT(_group != resources().end(),
        "Utility::Resource: group" << '\'' + group + '\'' << "was not found", );

    if(!_group->second.overrideGroup.empty()) {
        Debug() << "Utility::Resource: group" << '\'' + group + '\''
                << "overriden with" << '\'' + _group->second.overrideGroup + '\'';
        _overrideGroup = new OverrideData(_group->second.overrideGroup);

        if(_overrideGroup->conf.value("group") != _group->first)
            Warning() << "Utility::Resource: overriden with different group, found"
                      << '\'' + _overrideGroup->conf.value("group") + '\''
                      << "but expected" << '\'' + group + '\'';
    }
}

Resource::~Resource() {
    delete _overrideGroup;
}

std::vector<std::string> Resource::list() const {
    CORRADE_INTERNAL_ASSERT(_group != resources().end());

    std::vector<std::string> result;
    result.reserve(_group->second.resources.size());
    for(auto it = _group->second.resources.begin(); it != _group->second.resources.end(); ++it)
        result.push_back(it->first);

    return result;
}

Containers::ArrayReference<const unsigned char> Resource::getRaw(const std::string& filename) const {
    CORRADE_INTERNAL_ASSERT(_group != resources().end());

    /* The group is overriden with live data */
    if(_overrideGroup) {
        /* The file is already loaded */
        auto it = _overrideGroup->data.find(filename);
        if(it != _overrideGroup->data.end())
            return {it->second.begin(), it->second.size()};

        /* Load the file and save it for later use. Linear search is not an
           issue, as this shouldn't be used in production code anyway. */
        std::vector<const ConfigurationGroup*> files = _overrideGroup->conf.groups("file");
        for(auto fit = files.begin(); fit != files.end(); ++fit) {
            const ConfigurationGroup* const file = *fit;

            const std::string name = file->hasValue("alias") ? file->value("alias") : file->value("filename");
            if(name != filename) continue;

            /* Load the file */
            bool success;
            Containers::Array<unsigned char> data;
            std::tie(success, data) = fileContents(Directory::join(Directory::path(_group->second.overrideGroup), file->value("filename")));
            /* No nullptr here -> issue */
            if(!success)
                #ifndef CORRADE_GCC45_COMPATIBILITY
                return nullptr;
                #else
                return {};
                #endif

            /* Save the file for later use and return */
            #ifndef CORRADE_GCC47_COMPATIBILITY
            it = _overrideGroup->data.emplace(filename, std::move(data)).first;
            #else
            it = _overrideGroup->data.insert(std::make_pair(filename, std::move(data))).first;
            #endif
            return {it->second.begin(), it->second.size()};
        }

        /* The file was not found, fallback to compiled-in ones */
        Warning() << "Utility::Resource::get(): file" << '\'' + filename + '\''
                  << "was not found in overriden group, fallback to compiled-in resources";
    }

    const auto it = _group->second.resources.find(filename);
    #ifndef CORRADE_GCC45_COMPATIBILITY
    CORRADE_ASSERT(it != _group->second.resources.end(),
        "Utility::Resource::get(): file" << '\'' + filename + '\'' << "was not found in group" << '\'' + _group->first + '\'', nullptr);
    #else
    CORRADE_ASSERT(it != _group->second.resources.end(),
        "Utility::Resource::get(): file" << '\'' + filename + '\'' << "was not found in group" << '\'' + _group->first + '\'', {});
    #endif

    return it->second;
}

std::string Resource::get(const std::string& filename) const {
    Containers::ArrayReference<const unsigned char> data = getRaw(filename);
    return data ? std::string(reinterpret_cast<const char*>(data.begin()), data.size()) : std::string();
}

std::pair<bool, Containers::Array<unsigned char>> Resource::fileContents(const std::string& filename) {
    if(!Directory::fileExists(filename)) {
        Error() << "    Error: cannot open file" << filename;
        #if !defined(CORRADE_GCC45_COMPATIBILITY) && !defined(CORRADE_MSVC2013_COMPATIBILITY)
        return {false, nullptr};
        #elif !defined(CORRADE_MSVC2013_COMPATIBILITY)
        return {false, {}};
        #else
        return std::pair<bool, Containers::Array<unsigned char>&&>{false, Containers::Array<unsigned char>{}};
        #endif
    }

    return {true, Directory::read(filename)};
}

std::string Resource::comment(const std::string& comment) {
    return "\n    /* " + comment + " */";
}

std::string Resource::hexcode(const std::string& data) {
    std::ostringstream out;
    out << std::hex;

    /* Each row is indented by four spaces and has newline at the end */
    for(std::size_t row = 0; row < data.size(); row += 15) {
        out << "\n    ";

        /* Convert all characters on a row to hex "0xab,0x01,..." */
        for(std::size_t end = std::min(row + 15, data.size()), i = row; i != end; ++i) {
            out << "0x" << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(static_cast<unsigned char>(data[i]))
                << ",";
        }
    }

    return out.str();
}

#ifndef DOXYGEN_GENERATING_OUTPUT
template<class T> std::string Resource::numberToString(const T& number) {
    return std::string(reinterpret_cast<const char*>(&number), sizeof(T));
}
#endif

}}