#include "harness.hpp"

#include <filesystem>
#include <string>

#include "fixture.hpp"
#include "trident/codec.hpp"
#include "trident/query.hpp"
#include "trident/rdfs.hpp"
#include "trident/store.hpp"
#include "trident/turtle.hpp"

using namespace trident;

namespace {

TripleStore load_small() {
    TripleStore store;
    load_turtle_file(store, std::string(TRIDENT_DATA_DIR) + "/small.ttl");
    store.build();
    return store;
}

std::string temp_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

}  // namespace

TEST(persist, plain_save_reopens_through_mmap_without_rebuild) {
    TripleStore original = load_small();
    const std::size_t triples = original.triple_count();
    std::string path = temp_path("trident_plain.trident");
    original.save(path, SaveOptions{StoreCodec::Plain});

    TripleStore reopened = TripleStore::open(path);
    CHECK(reopened.built());
    CHECK_EQ(reopened.triple_count(), triples);
    CHECK(reopened.index(IndexOrder::Spo).is_mapped());
    CHECK(reopened.index(IndexOrder::Pos).is_mapped());
    CHECK(reopened.index(IndexOrder::Osp).is_mapped());

    EncodedPattern by_predicate;
    by_predicate.p = reopened.encode(Term::iri("http://example.org/name"));
    CHECK_EQ(reopened.count(by_predicate), std::size_t{4});

    QueryOutcome outcome = fixture::ask(reopened, "SELECT ?n WHERE { ?p ex:name ?n }");
    CHECK_EQ(outcome.results.size(), std::size_t{4});
    std::filesystem::remove(path);
}

TEST(persist, compressed_save_is_smaller_and_round_trips) {
    TripleStore original = load_small();
    std::string plain_path = temp_path("trident_plain_cmp.trident");
    std::string compressed_path = temp_path("trident_compressed.trident");
    original.save(plain_path, SaveOptions{StoreCodec::Plain});
    original.save(compressed_path, SaveOptions{StoreCodec::Compressed});

    auto file_size = [](const std::string& path) {
        return std::filesystem::file_size(path);
    };
    CHECK(file_size(compressed_path) < file_size(plain_path));

    TripleStore reopened = TripleStore::open(compressed_path);
    CHECK(reopened.built());
    CHECK(!reopened.index(IndexOrder::Spo).is_mapped());
    CHECK_EQ(reopened.triple_count(), original.triple_count());
    CHECK_EQ(fixture::ask(reopened, "SELECT ?n WHERE { ?p ex:name ?n }").results.size(),
             std::size_t{4});

    std::filesystem::remove(plain_path);
    std::filesystem::remove(compressed_path);
}

TEST(persist, codec_round_trips_an_index) {
    TripleStore store = load_small();
    const PermutedIndex& spo = store.index(IndexOrder::Spo);
    std::vector<std::uint8_t> bytes = compress_index(spo);
    std::vector<Triple> decoded = decompress_index(bytes.data(), bytes.size(), spo.permutation());
    CHECK_EQ(decoded.size(), spo.size());
    for (std::size_t i = 0; i < spo.size(); ++i) {
        CHECK(decoded[i] == spo[i]);
    }
}

TEST(persist, named_graphs_survive_save_and_open) {
    TripleStore store;
    store.add(Term::iri("http://a/s"), Term::iri("http://a/p"), Term::iri("http://a/o"));
    store.add(Term::iri("http://a/s2"), Term::iri("http://a/p"), Term::iri("http://a/o2"),
              Term::iri("http://a/g1"));
    store.build();
    std::string path = temp_path("trident_named.trident");
    store.save(path, SaveOptions{StoreCodec::Compressed});

    TripleStore reopened = TripleStore::open(path);
    CHECK_EQ(reopened.triple_count(), std::size_t{1});
    CHECK_EQ(reopened.quad_count(), std::size_t{2});
    CHECK_EQ(reopened.named_graphs().size(), std::size_t{1});
    std::filesystem::remove(path);
}
