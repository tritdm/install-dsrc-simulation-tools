//==========================================================================
// NEDRESOURCECACHE.CC -
//
//                     OMNeT++/OMNEST
//            Discrete System Simulation in C++
//
//==========================================================================

/*--------------------------------------------------------------*
  Copyright (C) 2002-2017 Andras Varga
  Copyright (C) 2006-2017 OpenSim Ltd.

  This file is distributed WITHOUT ANY WARRANTY. See the file
  `license' for details on this and other legal matters.
*--------------------------------------------------------------*/

#include <cstdio>
#include <cstring>
#include "common/fileutil.h"
#include "common/stringutil.h"
#include "common/stlutil.h"
#include "common/fileglobber.h"
#include "common/patternmatcher.h"
#include "common/opp_ctype.h"
#include "common/stringtokenizer.h"
#include "exception.h"
#include "nedresourcecache.h"

#include "errorstore.h"
#include "nedparser.h"
#include "neddtdvalidator.h"
#include "nedsyntaxvalidator.h"
#include "nedcrossvalidator.h"
#include "xmlastparser.h"

using namespace omnetpp::common;

namespace omnetpp {
namespace nedxml {

inline std::string canonicalize(const char *pathname)
{
    return tidyFilename(toAbsolutePath(pathname).c_str(), true);
}

NedResourceCache::~NedResourceCache()
{
    for (auto & file : nedFiles)
        delete file.second;
    for (auto & nedType : nedTypes)
        delete nedType.second;
}

void NedResourceCache::registerBuiltinDeclarations()
{
    // NED code to define built-in types
    const char *nedCode = NedParser::getBuiltInDeclarations();

    ErrorStore errors;
    NedParser parser(&errors);
    ASTNode *tree = parser.parseNedText(nedCode, "built-in-declarations");
    if (errors.containsError()) {
        delete tree;
        throw NedException("%s", getFirstError(&errors).c_str());
    }
    NedFileElement *nedFileElement = dynamic_cast<NedFileElement*>(tree);
    Assert(nedFileElement);

    // note: file must be called package.ned so that @namespace takes effect
    addFile("/[built-in-declarations]/package.ned", nedFileElement);
}

static std::vector<std::string> resolvePath(const char *folder, const char *path)
{
    PushDir pushDir(folder); // so that relative paths are interpreted as relative to the given folder
    std::vector<std::string> result;
    StringTokenizer tokenizer(path, PATH_SEPARATOR);
    while (tokenizer.hasMoreTokens()) {
        const char *item = tokenizer.nextToken();
        std::string folder = canonicalize(item);
        if (fileExists(folder.c_str()) && !contains(result, folder))
            result.push_back(folder);
    }
    return result;
}

int NedResourceCache::loadNedSourceFolder(const char *folderName, const char *excludedPackagesStr)
{
    try {
        std::vector<std::string> excludedPackages;
        for (std::string pkg : opp_splitandtrim(opp_nulltoempty(excludedPackagesStr), ";"))
            if (!pkg.empty())
                excludedPackages.push_back(pkg);
        std::string canonicalFolderName = canonicalize(folderName);
        std::string rootPackageName = determineRootPackageName(folderName);
        folderPackages[canonicalFolderName] = rootPackageName;
        return doLoadNedSourceFolder(folderName, rootPackageName.c_str(), excludedPackages);
    }
    catch (std::exception& e) {
        throw NedException("Could not load NED sources from '%s': %s", folderName, e.what());
    }
}

int NedResourceCache::doLoadNedSourceFolder(const char *folderName, const char *expectedPackage, const std::vector<std::string>& excludedPackages)
{
    if (!opp_isempty(expectedPackage) && contains(excludedPackages, std::string(expectedPackage)))  // note: the root package "" cannot be excluded
        return 0;

    PushDir pushDir(folderName);
    int count = 0;

    FileGlobber globber("*");
    const char *filename;
    while ((filename = globber.getNext()) != nullptr) {
        if (filename[0] == '.') {
            continue;  // ignore ".", "..", and dotfiles
        }
        if (isDirectory(filename)) {
            count += doLoadNedSourceFolder(filename, expectedPackage == nullptr ? nullptr : opp_join(".", expectedPackage, filename).c_str(), excludedPackages);
        }
        else if (opp_stringendswith(filename, ".ned")) {
            doLoadNedFileOrText(filename, nullptr, expectedPackage, false);
            count++;
        }
    }
    return count;
}

inline bool isPackageNedFile(const char *fname)
{
    return strcmp(fname, "package.ned") == 0 || opp_stringendswith(fname, "/package.ned");
}

void NedResourceCache::doLoadNedFileOrText(const char *nedFilename, const char *nedText, const char *expectedPackage, bool isXML)
{
    // checks
    Assert(nedFilename);
    std::string canonicalFilename = nedText ? nedFilename : canonicalize(nedFilename);  // so that NedFileElement stores absolute file name
    if (containsKey(nedFiles, canonicalFilename))
        return; // already loaded

    if (doneLoadingNedFilesCalled && isPackageNedFile(canonicalFilename.c_str()))
        throw NedException("Cannot load %s: 'package.ned' files can no longer be loaded at this point", canonicalFilename.c_str()); // as it could contain e.g. @namespace

    // parse file
    NedFileElement *tree = parseAndValidateNedFileOrText(canonicalFilename.c_str(), nedText, isXML);
    Assert(tree);

    // check that declared package matches expected package
    PackageElement *packageDecl = (PackageElement *)tree->getFirstChildWithTag(NED_PACKAGE);
    std::string declaredPackage = packageDecl ? packageDecl->getName() : "";
    if (expectedPackage != nullptr && declaredPackage != std::string(expectedPackage))
        throw NedException("Declared package '%s' does not match expected package '%s' in file %s",
                declaredPackage.c_str(), expectedPackage, nedFilename);

    // register it
    addFile(canonicalFilename.c_str(), tree);

    // if doneLoadingNedFiles() has already been called, we cannot defer resolving the types in it
    if (doneLoadingNedFilesCalled) {
        std::string packagePrefix = declaredPackage.empty() ? "" : declaredPackage + ".";
        collectNedTypesFrom(tree, packagePrefix, false);
        registerPendingNedTypes();
    }
}

NedFileElement *NedResourceCache::parseAndValidateNedFileOrText(const char *fname, const char *nedText, bool isXML)
{
    // load file
    ASTNode *tree = nullptr;
    ErrorStore errors;
    if (isXML) {
        if (nedText)
            throw NedException("loadNedText(): Parsing XML from string not supported");
        tree = parseXML(fname, &errors);
    }
    else {
        NedParser parser(&errors);
        parser.setStoreSource(false);
        if (nedText)
            tree = parser.parseNedText(nedText, fname);
        else
            tree = parser.parseNedFile(fname);
    }
    if (errors.containsError()) {
        delete tree;
        throw NedException("%s", getFirstError(&errors).c_str());
    }

    // DTD validation and additional syntax validation
    NedDtdValidator dtdvalidator(&errors);
    dtdvalidator.validate(tree);
    if (errors.containsError()) {
        delete tree;
        throw NedException("%s", getFirstError(&errors, "NED internal DTD validation failure: ").c_str());
    }

    NedSyntaxValidator syntaxvalidator(&errors);
    syntaxvalidator.validate(tree);
    if (errors.containsError()) {
        delete tree;
        throw NedException("%s", getFirstError(&errors).c_str());
    }
    NedFileElement *nedFileElement = dynamic_cast<NedFileElement*>(tree);
    if (!nedFileElement)
        throw NedException("<ned-file> expected as root element, in file %s", fname);
    return nedFileElement;
}

std::string NedResourceCache::getFirstError(ErrorStore *errors, const char *prefix)
{
    // find first error
    int i;
    for (i = 0; i < errors->numMessages(); i++)
        if (errors->errorSeverityCode(i) == SEVERITY_ERROR)
            break;
    Assert(i != errors->numMessages());

    // assemble message
    std::string message = errors->errorText(i);
    message[0] = opp_toupper(message[0]);
    std::string location = errors->errorLocation(i);
    if (opp_stringbeginswith(message.c_str(), "Syntax error, unexpected")) // this message is not really useful, replace it
        message = "Syntax error";
    if (!location.empty())
        message += ", at " + location;
    if (prefix)
        message = std::string(prefix) + message;
    return message;
}

void NedResourceCache::loadNedFile(const char *nedFilename, const char *expectedPackage, bool isXML)
{
    if (!nedFilename)
        throw NedException("loadNedFile(): File name is nullptr");

    doLoadNedFileOrText(nedFilename, nullptr, expectedPackage, isXML);
}

void NedResourceCache::loadNedText(const char *name, const char *nedText, const char *expectedPackage, bool isXML)
{
    if (!name)
        throw NedException("loadNedText(): Name is nullptr");
    doLoadNedFileOrText(name, nedText, expectedPackage, isXML);
}

void NedResourceCache::addFile(const char *nedFilename, NedFileElement *node)
{
    Assert(!containsKey(nedFiles, std::string(nedFilename)));
    //Assert(opp_streq(nedFilename, node->getFilename())); // would fail for "[built-in-declarations]"
    nedFiles[nedFilename] = node;
}

void NedResourceCache::collectNedTypesFrom(ASTNode *node, const std::string& packagePrefix, bool areInnerTypes)
{
    for (ASTNode *child = node->getFirstChild(); child; child = child->getNextSibling()) {
        int tag = child->getTagCode();
        if (tag == NED_CHANNEL || tag == NED_CHANNEL_INTERFACE || tag == NED_SIMPLE_MODULE ||
            tag == NED_COMPOUND_MODULE || tag == NED_MODULE_INTERFACE)
        {
            std::string qname = packagePrefix + child->getAttribute("name");
            collectNedType(qname.c_str(), areInnerTypes, child);

            if (ASTNode *types = child->getFirstChildWithTag(NED_TYPES))
                collectNedTypesFrom(types, qname+".", true);
        }
    }
}

void NedResourceCache::collectNedType(const char *qname, bool isInnerType, ASTNode *node)
{
    // we'll process it later, from doneLoadingNedFiles()
    pendingList.push_back(PendingNedType(qname, isInnerType, node));
}

bool NedResourceCache::areDependenciesResolved(const char *qname, ASTNode *node)
{
    // check that all base types are resolved
    NedLookupContext context = getParentContextOf(qname, node);
    for (ASTNode *child = node->getFirstChild(); child; child = child->getNextSibling()) {
        if (child->getTagCode() != NED_EXTENDS && child->getTagCode() != NED_INTERFACE_NAME)
            continue;

        const char *name = child->getAttribute("name");
        std::string qname = resolveNedType(context, name);
        if (qname.empty())
            return false;
    }
    return true;
}

void NedResourceCache::doneLoadingNedFiles()
{
    if (doneLoadingNedFilesCalled)
        throw NedException("NedResourceCache::doneLoadingNedFiles() may only be called once");
    doneLoadingNedFilesCalled = true;

    // collect package.ned files
    for (auto& entry : nedFiles) {
        NedFileElement *nedFile = entry.second;
        const char *fileName = nedFile->getFilename();
        if (isPackageNedFile(fileName)) {
            std::string packageName;
            if (PackageElement *packageDecl = (PackageElement *)nedFile->getFirstChildWithTag(NED_PACKAGE))
                packageName = packageDecl->getName();
            if (containsKey(packageDotNedFiles, packageName))
                throw NedException("More than one package.ned file for package '%s'%s: '%s' and '%s'",
                        packageName.c_str(), (packageName.empty() ? " (the default package)" : ""),
                        fileName, packageDotNedFiles[packageName]->getFilename());
            packageDotNedFiles[packageName] = nedFile;
        }
    }

    // collect types from loaded NED files
    for (auto& entry : nedFiles) {
        NedFileElement *nedFile = entry.second;
        std::string packagePrefix;
        if (PackageElement *packageDecl = nedFile->getFirstPackageChild())
            packagePrefix = std::string(packageDecl->getName()) + ".";
        collectNedTypesFrom(nedFile, packagePrefix, false);
    }

    // register NED types from all the files we've loaded
    registerPendingNedTypes();
}

void NedResourceCache::registerPendingNedTypes()
{
    bool again = true;
    while (again) {
        again = false;
        for (int i = 0; i < (int)pendingList.size(); i++) {
            PendingNedType type = pendingList[i];
            if (areDependenciesResolved(type.qname.c_str(), type.node)) {
                if (lookup(type.qname.c_str()))
                    throw NedException(type.node, "Redeclaration of %s %s", type.node->getTagName(), type.qname.c_str());

                registerNedType(type.qname.c_str(), type.isInnerType, type.node);
                pendingList.erase(pendingList.begin() + i--);
                again = true;
            }
        }
    }

    // report errors
    if (!pendingList.empty()) {
        std::string unresolvedNames;
        for (int i = 0; i < (int)pendingList.size(); i++)
            unresolvedNames += std::string(i == 0 ? "" : ", ") + pendingList[i].qname;
        if (pendingList.size() == 1)
            throw NedException(pendingList[0].node, "NED type '%s' could not be fully resolved due to a missing base type or interface", unresolvedNames.c_str());
        else
            throw NedException("The following NED types could not be fully resolved due to a missing base type or interface: %s", unresolvedNames.c_str());
    }
}

void NedResourceCache::registerNedType(const char *qname, bool isInnerType, ASTNode *node)
{
    NedTypeInfo *decl = new NedTypeInfo(this, qname, isInnerType, node);
    nedTypes[qname] = decl;
    nedTypeNames.clear();  // invalidate
}

NedTypeInfo *NedResourceCache::lookup(const char *qname) const
{
    // hash table lookup
    auto it = nedTypes.find(qname);
    return it == nedTypes.end() ? nullptr : it->second;
}

NedTypeInfo *NedResourceCache::getDecl(const char *qname) const
{
    NedTypeInfo *decl = lookup(qname);
    if (!decl)
        throw NedException("NED declaration '%s' not found", qname);
    return decl;
}

inline std::string getParentPackage(std::string& package)
{
    int pos = package.rfind('.');
    return (pos != -1) ? package.substr(0, pos) : "";
}

std::vector<NedFileElement*> NedResourceCache::getPackageNedListForLookup(const char *packageName) const
{
    std::vector<NedFileElement*> result;
    std::string package = packageName;
    while (true) {
        if (containsKey(packageDotNedFiles, package))
            result.push_back(packageDotNedFiles.at(package));
        if (package == "")
            break;
        package = getParentPackage(package);
    }
    return result;
}

std::string NedResourceCache::determineRootPackageName(const char *nedSourceFolderName)
{
    // determine if a package.ned file exists
    std::string packageNedFilename = std::string(nedSourceFolderName) + "/package.ned";
    if (!fileExists(packageNedFilename.c_str()))
        return "";

    // read package declaration from it
    ASTNode *tree = parseAndValidateNedFileOrText(packageNedFilename.c_str(), nullptr, false);
    Assert(tree);
    PackageElement *packageDecl = (PackageElement *)tree->getFirstChildWithTag(NED_PACKAGE);
    std::string result = packageDecl ? packageDecl->getName() : "";
    delete tree;
    return result;
}

static bool isPathPrefixOf(const char *prefix, const char *path)
{
    // note: both "prefix" and "path" must be canonical absolute paths for this to work
    int pathlen = strlen(path);
    int prefixlen = strlen(prefix);
    Assert(prefix[prefixlen-1] != '/' && path[pathlen-1] != '/');
    if (pathlen == prefixlen)
        return strcmp(path, prefix) == 0;
    else if (pathlen < prefixlen)
        return false;  // too short
    else if (strncmp(path, prefix, strlen(prefix)) != 0)
        return false;  // differ
    else
        return path[prefixlen] == '/';  // e.g. "/tmp/foo" is not prefix of "/tmp/foolish"
}

std::string NedResourceCache::getNedSourceFolderForFolder(const char *folder) const
{
    // find NED source folder which is a prefix of folder.
    // note: this is unambiguous because nested NED source folders are not allowed
    std::string folderName = canonicalize(folder);
    for (const auto & folderPackage : folderPackages)
        if (isPathPrefixOf(folderPackage.first.c_str(), folderName.c_str()))
            return folderPackage.first;

    return "";
}

std::string NedResourceCache::getNedPackageForFolder(const char *folder) const
{
    std::string sourceFolder = getNedSourceFolderForFolder(folder);
    if (sourceFolder.empty())
        return "";

    std::string folderName = canonicalize(folder);
    std::string suffix = folderName.substr(sourceFolder.size());
    if (suffix.length() > 0 && suffix[0] == '/')
        suffix = suffix.substr(1);
    std::string subpackage = opp_replacesubstring(suffix, "/", ".", true);
    return opp_join(".", const_cast<StringMap&>(folderPackages)[sourceFolder].c_str(), subpackage.c_str());
}

NedLookupContext NedResourceCache::getParentContextOf(const char *qname, ASTNode *node)
{
    ASTNode *contextNode = node->getParent();
    if (contextNode->getTagCode() == NED_TYPES)
        contextNode = contextNode->getParent();
    const char *lastdot = strrchr(qname, '.');
    std::string contextQName = !lastdot ? "" : std::string(qname, lastdot-qname);
    return NedLookupContext(contextNode, contextQName.c_str());
}

std::string NedResourceCache::resolveNedType(const NedLookupContext& context, const char *nedTypeName, INedTypeNames *qnames)
{
    // note: this method is to be kept consistent with NedResources.lookupNedType() in the Java code
    // note2: partially qualified names are not supported: name must be either simplename or fully qualified
    if (!strchr(nedTypeName, '.')) {
        // no dot: name is an unqualified name (simple name); so, it can be:
        // (a) inner type, (b) an exactly imported type, (c) from the same package, (d) a wildcard imported type

        // inner type?
        if (context.element->getTagCode() == NED_COMPOUND_MODULE) {
            std::string qname = context.qname;
            bool contextIsInnerType = context.element->getParent()->getParentWithTag(NED_COMPOUND_MODULE) != nullptr;
            if (contextIsInnerType) {
                // if context is already an inner type, look up nedtypename in its enclosing toplevel NED type
                int index = qname.rfind('.');
                Assert(index != -1);
                qname = qname.substr(0, index);
            }
            qname = qname + "." + nedTypeName;
            if (qnames->contains(qname.c_str()))
                return qname;
            // TODO: try with ancestor types (i.e. maybe nedTypeName is an inherited inner type)
        }

        NedFileElement *nedfileNode = dynamic_cast<NedFileElement *>(context.element->getParentWithTag(NED_NED_FILE));

        // collect imports, for convenience
        std::vector<const char *> imports;
        for (ImportElement *import = nedfileNode->getFirstImportChild(); import; import = import->getNextImportSibling())
            imports.push_back(import->getImportSpec());

        // exactly imported type?
        // try a shortcut first: if the import doesn't contain wildcards
        std::string dot_nedtypename = std::string(".")+nedTypeName;
        for (auto & import : imports)
            if (qnames->contains(import) && (opp_stringendswith(import, dot_nedtypename.c_str()) || strcmp(import, nedTypeName) == 0))
                return import;


        // from the same package?
        PackageElement *packageNode = nedfileNode->getFirstPackageChild();
        const char *packageName = packageNode ? packageNode->getName() : "";
        std::string qname = opp_isempty(packageName) ? nedTypeName : std::string(packageName) + "." + nedTypeName;
        if (qnames->contains(qname.c_str()))
            return qname;

        // try harder, using wildcards
        for (auto & import : imports) {
            if (PatternMatcher::containsWildcards(import)) {
                PatternMatcher importPattern(import, true, true, true);
                for (int j = 0; j < qnames->size(); j++) {
                    const char *qname = qnames->get(j);
                    if ((opp_stringendswith(qname, dot_nedtypename.c_str()) || strcmp(qname, nedTypeName) == 0))
                        if (importPattern.matches(qname))
                            return qname;

                }
            }
        }
    }
    else {
        // fully qualified name?
        if (qnames->contains(nedTypeName))
            return nedTypeName;
    }

    return "";
}

const std::vector<std::string>& NedResourceCache::getTypeNames() const
{
    if (nedTypeNames.empty() && !nedTypes.empty()) {
        // fill in nedTypeNames vector
        for (const auto & nedType : nedTypes)
            nedTypeNames.push_back(nedType.first);
    }
    return nedTypeNames;
}

}  // namespace nedxml
}  // namespace omnetpp

