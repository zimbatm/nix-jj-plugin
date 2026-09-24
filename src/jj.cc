#include "nix/fetchers/cache.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/fetchers.hh"
#include "nix/store/store-api.hh"
#include "nix/util/file-system.hh"
#include "nix/util/os-string.hh"
#include "nix/util/processes.hh"
#include "nix/util/url-parts.hh"
#include "nix/util/users.hh"

namespace nix::fetchers {

/* Run in the workspace rather than passing `--repository`: that option names
   the repository, while the working copy to snapshot is the one the current
   directory sits in, and a second workspace shares its repository with the
   one that created it. */
static std::string runJJ(const std::filesystem::path & workspace, OsStrings args)
{
    OsStrings full{OS_STR("--color"), OS_STR("never"), OS_STR("--quiet")};
    for (auto & arg : args)
        full.push_back(std::move(arg));

    auto res = runProgram(
        {.program = "jj", .lookupPath = true, .args = std::move(full), .chdir = workspace});

    if (!statusOk(res.first))
        throw ExecError(res.first, "jj %1%", statusToString(res.first));

    return res.second;
}

/* `jj file list` renders one entry per template expansion, so a NUL
   separator keeps paths with newlines in them intact. */
static StringSet trackedFiles(const std::filesystem::path & workspace, const std::string & rev)
{
    using namespace std::string_literals;
    auto out = runJJ(
        workspace,
        {OS_STR("file"),
         OS_STR("list"),
         OS_STR("-r"),
         string_to_os_string(rev),
         OS_STR("-T"),
         OS_STR("path ++ \"\\0\"")});
    return tokenizeString<StringSet>(out, "\0"s);
}

static std::string logField(const std::filesystem::path & workspace, const std::string & rev, const std::string & field)
{
    return chomp(runJJ(
        workspace,
        {OS_STR("log"),
         OS_STR("--no-graph"),
         OS_STR("-r"),
         string_to_os_string(rev),
         OS_STR("-T"),
         string_to_os_string(field)}));
}

struct JJInputScheme : InputScheme
{
    std::optional<Input> inputFromURL(const ParsedURL & url, bool requireTree) const override
    {
        if (url.scheme != "jj+file")
            return {};

        auto url2(url);
        url2.scheme = "file";
        url2.query.clear();

        Attrs attrs;
        attrs.emplace("type", "jj");
        attrs.emplace("url", url2.to_string());

        return inputFromAttrs(attrs);
    }

    std::string_view schemeName() const override
    {
        return "jj";
    }

    std::optional<ParsedURL> localRepoURL(const std::filesystem::path & path) const override
    {
        if (!pathExists(path / ".jj"))
            return std::nullopt;
        return ParsedURL{
            .scheme = "jj+file",
            .authority = ParsedURL::Authority{},
            .path = pathToUrlPath(path),
        };
    }

    std::string schemeDescription() const override
    {
        return stripIndentation(R"(
          Fetch a Jujutsu workspace and copy it to the Nix store.
        )");
    }

    const std::map<std::string, AttributeInfo> & allowedAttrs() const override
    {
        static const std::map<std::string, AttributeInfo> attrs = {
            {"url", {}},
            {"rev", {}},
            {"narHash", {}},
            {"name", {}},
        };
        return attrs;
    }

    std::optional<Input> inputFromAttrs(const Attrs & attrs) const override
    {
        parseURL(getStrAttr(attrs, "url"));

        Input input{};
        input.attrs = attrs;
        return input;
    }

    ParsedURL toURL(const Input & input) const override
    {
        auto url = parseURL(getStrAttr(input.attrs, "url"));
        url.scheme = "jj+" + url.scheme;
        if (auto rev = input.getRev())
            url.query.insert_or_assign("rev", rev->gitRev());
        return url;
    }

    std::filesystem::path getPath(const Input & input) const
    {
        auto url = parseURL(getStrAttr(input.attrs, "url"));
        if (url.scheme != "file")
            throw Error("Jujutsu inputs only support local workspaces, not '%s'", url.to_string());
        return urlPathToPath(url.path);
    }

    std::optional<std::filesystem::path> getSourcePath(const Input & input) const override
    {
        if (!input.getRev())
            return getPath(input);
        return {};
    }

    StorePath fetchToStore(const Settings & settings, Store & store, Input & input) const
    {
        auto path = getPath(input);
        auto name = input.getName();

        /* Any jj command snapshots the working copy first, so `@` already
           carries the files as they are on disk. A `@` that is empty against
           its parent is a clean checkout, and `@-` is then the revision to
           report; otherwise this is the working copy of a change in progress,
           which no revision can name. */
        auto clean = logField(path, "@", "empty") == "true";
        std::optional<Cache::Key> cacheKey;

        if (clean) {
            if (!input.getRev())
                input.attrs.insert_or_assign("rev", logField(path, "@-", "commit_id"));

            cacheKey = Cache::Key{
                "jjRev", {{"store", store.storeDir}, {"name", name}, {"rev", input.getRev()->gitRev()}}};

            if (auto res = settings.getCache()->lookupStorePath(*cacheKey, store))
                return std::move(res->storePath);
        } else {
            if (!settings.allowDirty)
                throw Error("Jujutsu workspace '%s' has a change in progress", PathFmt{path});
            if (settings.warnDirty)
                warn("Jujutsu workspace '%s' has a change in progress", PathFmt{path});
        }

        auto files = trackedFiles(path, "@");
        auto accessor = makeFSSourceAccessor(absPath(path));

        PathFilter filter = [&](const std::string & p) -> bool {
            auto cp = CanonPath(p);
            if (accessor->lstat(cp).type == SourceAccessor::tDirectory) {
                auto prefix = cp.rel() + "/";
                auto i = files.lower_bound(prefix);
                return i != files.end() && hasPrefix(*i, prefix);
            }
            return files.count(cp.rel());
        };

        auto storePath = store.addToStore(
            name,
            {accessor, CanonPath::root},
            ContentAddressMethod::Raw::NixArchive,
            HashAlgorithm::SHA256,
            {},
            filter);

        if (cacheKey)
            settings.getCache()->upsert(*cacheKey, store, {}, storePath);

        return storePath;
    }

    std::pair<ref<SourceAccessor>, Input>
    getAccessor(const Settings & settings, Store & store, const Input & _input) const override
    {
        Input input(_input);

        auto storePath = fetchToStore(settings, store, input);
        auto accessor = store.requireStoreObjectAccessor(storePath);

        accessor->setPathDisplay("«" + input.to_string() + "»");

        return {accessor, input};
    }

    bool isLocked(const Settings & settings, const Input & input) const override
    {
        return (bool) input.getRev();
    }

    std::optional<std::string> getFingerprint(Store & store, const Input & input) const override
    {
        if (auto rev = input.getRev())
            return rev->gitRev();
        return std::nullopt;
    }
};

} // namespace nix::fetchers

extern "C" void nix_plugin_entry()
{
    nix::fetchers::registerInputScheme(std::make_shared<nix::fetchers::JJInputScheme>());
}
