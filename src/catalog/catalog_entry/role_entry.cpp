#include "duckdb/catalog/catalog_entry/role_entry.hpp"

namespace duckdb {

RoleEntry::RoleEntry(Catalog &catalog, Identifier name, idx_t oid, RoleOption options_p, vector<Membership> member_of_p,
                     RoleCapability capabilities_p, bool predefined_p)
    : InCatalogEntry(CatalogType::ROLE_ENTRY, catalog, std::move(name), oid), options(options_p),
      member_of(std::move(member_of_p)), capabilities(capabilities_p), predefined(predefined_p) {
}

} // namespace duckdb
