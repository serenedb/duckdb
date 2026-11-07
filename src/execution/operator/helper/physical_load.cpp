#include "duckdb/execution/operator/helper/physical_load.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_repository_manager.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

static void InstallFromRepository(ClientContext &context, const LoadInfo &info) {
	auto &db = DatabaseInstance::GetDatabase(context);
	auto &fs = FileSystem::GetLocal(db);

	ExtensionRepository repository;
	// the repository can be a trusted repository that was added by the user, a built-in repository name or a url
	if (!ExtensionRepositoryManager::TryGetRepository(db, fs, info.repository, repository)) {
		if (info.repo_is_alias) {
			// This has been checked during bind, so it should not fail here
			if (!ExtensionRepository::TryGetKnownRepository(info.repository, repository)) {
				throw InternalException("The repository alias failed to resolve");
			}
		} else {
			repository = ExtensionRepository::GetRepositoryByUrl(info.repository);
		}
	}

	ExtensionInstallOptions options;
	options.force_install = info.load_type == LoadType::FORCE_INSTALL;
	options.throw_on_origin_mismatch = true;
	options.version = info.version;
	options.repository = repository;

	ExtensionHelper::InstallExtension(context, info.filename, options);
}

static void ExecuteRepositoryStatement(ClientContext &context, const LoadInfo &info, DataChunk &chunk) {
	auto &db = DatabaseInstance::GetDatabase(context);
	auto &fs = FileSystem::GetLocal(db);

	if (info.load_type == LoadType::CREATE_REPOSITORY) {
		ExtensionRepository repository(info.repository, info.repository_url, info.public_keys);
		auto result = ExtensionRepositoryManager::CreateRepository(db, fs, context, repository, info.on_conflict);

		// report the repository as it was stored, so that the keys can be compared with the published fingerprints
		vector<Value> fingerprints;
		for (auto &public_key : result.public_keys) {
			fingerprints.push_back(Value(ExtensionRepositoryManager::GetPublicKeyFingerprint(public_key)));
		}
		chunk.data[0].Append(Value(result.name));
		chunk.data[1].Append(Value(result.path));
		chunk.data[2].Append(Value::LIST(LogicalType::VARCHAR, std::move(fingerprints)));
	} else {
		auto on_entry_not_found = info.missing_ok ? OnEntryNotFound::RETURN_NULL : OnEntryNotFound::THROW_EXCEPTION;
		ExtensionRepositoryManager::DropRepository(db, fs, info.repository, on_entry_not_found);
	}
}

SourceResultType PhysicalLoad::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                               OperatorSourceInput &input) const {
	if (info->load_type == LoadType::CREATE_REPOSITORY || info->load_type == LoadType::DROP_REPOSITORY) {
		ExecuteRepositoryStatement(context.client, *info, chunk);
		return SourceResultType::FINISHED;
	}

	// The binder only lets a statement reach here when the extension is compiled
	// into the binary. INSTALL then has nothing to do; LOAD still has to register
	// it with this database, which LoadStaticExtension does from the linked-in
	// registry -- no download, and a no-op when it is already loaded. The upstream
	// paths below stay for builds that allow runtime loading; here they would only
	// turn an accepted statement back into an error.
	const auto extension_name = ExtensionHelper::GetExtensionName(info->filename);
	if (ExtensionHelper::IsLinkedExtension(extension_name)) {
		const bool is_install = info->load_type == LoadType::INSTALL || info->load_type == LoadType::FORCE_INSTALL;
		if (!is_install || info->load_after_install) {
			DuckDB db_wrapper(*context.client.db);
			ExtensionHelper::LoadExtension(db_wrapper, extension_name);
			ExtensionLoader::RefreshSearchPath(context.client);
		}
		return SourceResultType::FINISHED;
	}

	if (info->load_type == LoadType::INSTALL || info->load_type == LoadType::FORCE_INSTALL) {
		if (info->repository.empty()) {
			ExtensionInstallOptions options;
			options.force_install = info->load_type == LoadType::FORCE_INSTALL;
			options.throw_on_origin_mismatch = true;
			options.version = info->version;
			ExtensionHelper::InstallExtension(context.client, info->filename, options);
		} else {
			InstallFromRepository(context.client, *info);
		}

		// INSTALL AND LOAD: load the extension immediately after installing it. Only a named repository (core,
		// community or a user repository alias) is passed on to the load - a URL or bare install lands in the flat
		// top-level layout, which a bare load resolves
		if (info->load_after_install) {
			ExtensionLoadOptions load_options;
			load_options.extension_name = info->filename;
			load_options.repository = info->repo_is_alias ? info->repository : string();
			ExtensionHelper::LoadExternalExtension(context.client, load_options);
			ExtensionLoader::RefreshSearchPath(context.client);
		}

	} else {
		ExtensionLoadOptions options;
		options.extension_name = info->filename;
		options.alias = info->alias;
		options.repository = info->repository;
		ExtensionHelper::LoadExternalExtension(context.client, options);
		// adds an explicitly set extension schema to the search path
		ExtensionLoader::RefreshSearchPath(context.client);
	}

	return SourceResultType::FINISHED;
}

} // namespace duckdb
