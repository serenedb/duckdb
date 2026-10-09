#include "duckdb/catalog/catalog_entry/replication_origin_catalog_entry.hpp"

namespace duckdb {

ReplicationOriginCatalogEntry::ReplicationOriginCatalogEntry(Catalog &catalog, CreateReplicationOriginInfo &info)
    : ReplicationLsnEntry(CatalogType::REPLICATION_ORIGIN_ENTRY, catalog, info.GetQualifiedName().Name(), info.oid,
                          make_shared_ptr<ReplicationLsnState>(info.remote_lsn)) {
	comment = info.comment;
	tags = info.tags;
	permissions = info.permissions;
}

unique_ptr<CreateInfo> ReplicationOriginCatalogEntry::GetInfo() const {
	auto info = make_uniq<CreateReplicationOriginInfo>();
	info->SetName(name);
	info->oid = oid;
	info->remote_lsn = RemoteLsn();
	info->permissions = permissions;
	info->comment = comment;
	info->tags = tags;
	return std::move(info);
}

unique_ptr<CatalogEntry> ReplicationOriginCatalogEntry::Copy(ClientContext &context) const {
	auto info = GetInfo();
	return make_uniq<ReplicationOriginCatalogEntry>(catalog, info->Cast<CreateReplicationOriginInfo>());
}

} // namespace duckdb
