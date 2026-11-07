#include "duckdb/main/extension_callback_manager.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/operator_extension.hpp"
#include "duckdb/planner/planner_extension.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "duckdb/main/profiler_extension.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

struct ExtensionCallbackRegistry {
	//! Extensions made to the planner
	vector<PlannerExtension> planner_extensions;
	//! Extensions made to the optimizer
	vector<OptimizerExtension> optimizer_extensions;
	//! Extensions made to binder
	vector<shared_ptr<OperatorExtension>> operator_extensions;
	//! Extensions made to storage
	case_insensitive_map_t<shared_ptr<StorageExtension>> storage_extensions;
	//! Set of callbacks that can be installed by extensions
	vector<shared_ptr<ExtensionCallback>> extension_callbacks;
	//! Pluggable profiler / EXPLAIN tree renderers, keyed by format name
	case_insensitive_map_t<shared_ptr<ProfilerExtension>> profiler_extensions;
};

ExtensionCallbackManager &ExtensionCallbackManager::Get(ClientContext &context) {
	return DBConfig::GetConfig(context).GetCallbackManager();
}

const ExtensionCallbackManager &ExtensionCallbackManager::Get(const ClientContext &context) {
	return DBConfig::GetConfig(context).GetCallbackManager();
}

ExtensionCallbackManager &ExtensionCallbackManager::Get(DatabaseInstance &db) {
	return DBConfig::GetConfig(db).GetCallbackManager();
}

ExtensionCallbackManager::ExtensionCallbackManager() {
	registry_versions.push_back(make_uniq<ExtensionCallbackRegistry>());
	callback_registry.store(registry_versions.back().get(), std::memory_order_release);
}

ExtensionCallbackRegistry &ExtensionCallbackManager::NextVersion() {
	registry_versions.push_back(make_uniq<ExtensionCallbackRegistry>(Current()));
	return *registry_versions.back();
}

void ExtensionCallbackManager::Publish(ExtensionCallbackRegistry &registry) {
	callback_registry.store(&registry, std::memory_order_release);
}

const ExtensionCallbackRegistry &ExtensionCallbackManager::Current() const {
	return *callback_registry.load(std::memory_order_acquire);
}
ExtensionCallbackManager::~ExtensionCallbackManager() {
}

void ExtensionCallbackManager::AddExtensionSchema(const Identifier &schema) {
	extension_schemas.push_back(schema.GetIdentifierName());
}

vector<string> ExtensionCallbackManager::GetExtensionSchemas() const {
	return extension_schemas;
}

void ExtensionCallbackManager::Register(PlannerExtension extension) {
	lock_guard<mutex> guard(registry_lock);
	auto &new_registry = NextVersion();
	new_registry.planner_extensions.push_back(std::move(extension));
	Publish(new_registry);
}

void ExtensionCallbackManager::Register(OptimizerExtension extension) {
	lock_guard<mutex> guard(registry_lock);
	auto &new_registry = NextVersion();
	new_registry.optimizer_extensions.push_back(std::move(extension));
	Publish(new_registry);
}

void ExtensionCallbackManager::Register(shared_ptr<OperatorExtension> extension) {
	lock_guard<mutex> guard(registry_lock);
	auto &new_registry = NextVersion();
	new_registry.operator_extensions.push_back(std::move(extension));
	Publish(new_registry);
}

void ExtensionCallbackManager::Register(const string &name, shared_ptr<StorageExtension> extension) {
	lock_guard<mutex> guard(registry_lock);
	auto &new_registry = NextVersion();
	new_registry.storage_extensions[name] = std::move(extension);
	Publish(new_registry);
}

void ExtensionCallbackManager::Register(shared_ptr<ExtensionCallback> extension) {
	lock_guard<mutex> guard(registry_lock);
	auto &new_registry = NextVersion();
	new_registry.extension_callbacks.push_back(std::move(extension));
	Publish(new_registry);
}

void ExtensionCallbackManager::Register(const string &name, shared_ptr<ProfilerExtension> extension) {
	lock_guard<mutex> guard(registry_lock);
	auto &new_registry = NextVersion();
	new_registry.profiler_extensions[name] = std::move(extension);
	Publish(new_registry);
}

template <class T>
ExtensionCallbackIteratorHelper<T>::ExtensionCallbackIteratorHelper(const vector<T> &vec) : vec(vec) {
}

template <class T>
ExtensionCallbackIteratorHelper<T>::~ExtensionCallbackIteratorHelper() {
}

ExtensionCallbackIteratorHelper<shared_ptr<OperatorExtension>> ExtensionCallbackManager::OperatorExtensions() const {
	return ExtensionCallbackIteratorHelper<shared_ptr<OperatorExtension>>(Current().operator_extensions);
}

ExtensionCallbackIteratorHelper<OptimizerExtension> ExtensionCallbackManager::OptimizerExtensions() const {
	return ExtensionCallbackIteratorHelper<OptimizerExtension>(Current().optimizer_extensions);
}

ExtensionCallbackIteratorHelper<PlannerExtension> ExtensionCallbackManager::PlannerExtensions() const {
	return ExtensionCallbackIteratorHelper<PlannerExtension>(Current().planner_extensions);
}

ExtensionCallbackIteratorHelper<shared_ptr<ExtensionCallback>> ExtensionCallbackManager::ExtensionCallbacks() const {
	return ExtensionCallbackIteratorHelper<shared_ptr<ExtensionCallback>>(Current().extension_callbacks);
}

optional_ptr<StorageExtension> ExtensionCallbackManager::FindStorageExtension(const string &name) const {
	auto &registry = Current();
	auto entry = registry.storage_extensions.find(name);
	if (entry == registry.storage_extensions.end()) {
		return nullptr;
	}
	return entry->second.get();
}

optional_ptr<ProfilerExtension> ExtensionCallbackManager::FindProfilerExtension(const string &name) const {
	auto &registry = Current();
	auto entry = registry.profiler_extensions.find(name);
	if (entry == registry.profiler_extensions.end()) {
		return nullptr;
	}
	return entry->second.get();
}

void OptimizerExtension::Register(DBConfig &config, OptimizerExtension extension) {
	config.GetCallbackManager().Register(std::move(extension));
}

void PlannerExtension::Register(DBConfig &config, PlannerExtension extension) {
	config.GetCallbackManager().Register(std::move(extension));
}

void OperatorExtension::Register(DBConfig &config, shared_ptr<OperatorExtension> extension) {
	config.GetCallbackManager().Register(std::move(extension));
}

optional_ptr<StorageExtension> StorageExtension::Find(const DBConfig &config, const string &extension_name) {
	return config.GetCallbackManager().FindStorageExtension(extension_name);
}

void ExtensionCallback::Register(DBConfig &config, shared_ptr<ExtensionCallback> extension) {
	config.GetCallbackManager().Register(std::move(extension));
}

void StorageExtension::Register(DBConfig &config, const string &extension_name,
                                shared_ptr<StorageExtension> extension) {
	config.GetCallbackManager().Register(extension_name, std::move(extension));
}

void ProfilerExtension::Register(DBConfig &config, const string &format_name, shared_ptr<ProfilerExtension> extension) {
	config.GetCallbackManager().Register(format_name, std::move(extension));
}

optional_ptr<ProfilerExtension> ProfilerExtension::Find(const ClientContext &context, const string &format_name) {
	return ExtensionCallbackManager::Get(context).FindProfilerExtension(format_name);
}

template class ExtensionCallbackIteratorHelper<shared_ptr<ExtensionCallback>>;
template class ExtensionCallbackIteratorHelper<shared_ptr<OperatorExtension>>;
template class ExtensionCallbackIteratorHelper<OptimizerExtension>;
template class ExtensionCallbackIteratorHelper<PlannerExtension>;

} // namespace duckdb
