//! Search the Wasmer registry.

use futures_util::StreamExt;
use wasmer_backend_api::WasmerClient;

pub use wasmer_backend_api::types::{
    CountComparison, CountFilter, PackageOrderBy, PackagesFilter, SearchOrderSort,
    SearchPackageVersion, SearchPublishDate,
};

/// Number of results fetched per registry request while paging.
const PAGE_SIZE: i32 = 50;

/// Options for [`search_packages`].
#[derive(Debug, Clone, Default)]
pub struct SearchOptions {
    /// Free-text query. An empty string (or `"*"`) matches everything.
    pub query: String,
    /// Additional filters, such as owner, curated status or download count.
    pub filter: PackagesFilter,
    /// Maximum number of results to return. `None` fetches every match.
    pub limit: Option<usize>,
}

/// Search the registry for packages.
///
/// Pages through the registry until `opts.limit` results have been collected
/// (or all matching packages, if no limit is set).
pub async fn search_packages(
    client: &WasmerClient,
    opts: SearchOptions,
) -> Result<Vec<SearchPackageVersion>, anyhow::Error> {
    let SearchOptions {
        query,
        filter,
        limit,
    } = opts;

    let mut stream = Box::pin(wasmer_backend_api::query::fetch_all_matching_packages(
        client,
        query,
        Some(filter),
        PAGE_SIZE,
    ));

    let mut results = Vec::new();
    while let Some(page) = stream.next().await {
        results.extend(page?);
        if let Some(limit) = limit
            && results.len() >= limit
        {
            results.truncate(limit);
            break;
        }
    }

    Ok(results)
}
