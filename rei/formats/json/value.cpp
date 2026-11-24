#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <new>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "rei/formats/json.hpp"
#include "rei/formats/json/detail.hpp"

namespace rei::formats::json {
    namespace detail {
        namespace {
            constexpr auto smallest_capacity = 8uz;

            struct extent {
                std::size_t slots{};
                std::size_t text{};
            };

            struct pending {
                const node* next;
                const node* last;
            };

            auto is_container(const node& held) noexcept -> bool {
                return kind_of(held) >= kind::array;
            }

            auto is_borrowed(const node& held) noexcept -> bool {
                if (kind_of(held) == kind::string) {
                    return (held.meta & (inline_bit | owner_bit)) == 0;
                }

                if (!is_container(held)) {
                    return false;
                }

                return (held.meta & tethered_bit) != 0 || (!is_owner(held) && size_of(held) != 0);
            }

            auto is_pristine(const node& held) noexcept -> bool {
                if (!is_owner(held)) {
                    return false;
                }

                const auto* owned = header_of(held).owned;
                return !is_loose(held) && owned != nullptr && !owned->dirty;
            }

            auto resize(node& held, std::size_t size) noexcept -> void {
                held.meta = (held.meta & tag_bits) | (static_cast<std::uint64_t>(size) << size_shift);
            }

            auto capacity_of(const node& held) noexcept -> std::size_t {
                return is_owner(held) ? header_of(held).capacity : slot_count(held);
            }

            auto reserve_slots(node& held, std::size_t wanted) -> void {
                if (is_owner(held) && header_of(held).capacity >= wanted) {
                    return;
                }

                const auto count = slot_count(held);
                const auto tethered = !is_owner(held) && count != 0 ? tethered_bit : 0;
                auto* owned = is_owner(held) ? header_of(held).owned : nullptr;
                auto* block = static_cast<node*>(allocate((wanted + 1) * sizeof(node)));
                if (count != 0) {
                    std::memcpy(static_cast<void*>(block + 1), slots_of(held), count * sizeof(node));
                }
                if (is_loose(held)) {
                    deallocate(&header_of(held));
                }

                ::new (static_cast<void*>(block)) header{.owned = owned, .capacity = wanted};
                held.slots = reinterpret_cast<json::value*>(block + 1);
                held.meta |= owner_bit | loose_bit | tethered;
            }

            auto room(node& held, std::size_t extra) -> node* {
                const auto needed = slot_count(held) + extra;
                const auto capacity = capacity_of(held);
                if (needed > capacity || !is_owner(held)) {
                    reserve_slots(held, std::max({needed, capacity * 2, smallest_capacity}));
                }

                return slots_of(held);
            }

            auto measure(const node& root) -> extent {
                if (is_pristine(root)) {
                    const auto& owner = *header_of(root).owned;
                    return {.slots = owner.slots, .text = owner.text};
                }

                extent total{.slots = slot_count(root)};
                std::vector<pending> trail{
                    {.next = slots_of(root), .last = slots_of(root) + slot_count(root)},
                };

                while (!trail.empty()) {
                    auto& level = trail.back();
                    if (level.next == level.last) {
                        trail.pop_back();
                        continue;
                    }

                    const auto& item = *level.next;
                    level.next++;

                    if (kind_of(item) == kind::string) {
                        total.text += (item.meta & inline_bit) != 0 ? 0 : size_of(item);
                    } else if (is_container(item) && size_of(item) != 0) {
                        total.slots += slot_count(item);
                        trail.push_back({.next = slots_of(item), .last = slots_of(item) + slot_count(item)});
                    }
                }

                return total;
            }

            auto clone_tree(const node& source) -> node {
                if (size_of(source) == 0) {
                    return {.meta = tag(kind_of(source)), .natural = 0};
                }

                const auto total = measure(source);
                const auto space = open_arena(((total.slots + 1) * sizeof(node)) + total.text + text_padding);
                auto* block = reinterpret_cast<node*>(space.low);
                auto* free = block + 1;
                auto* text = reinterpret_cast<char*>(free + total.slots);

                ::new (static_cast<void*>(block))
                    header{.owned = space.owner, .capacity = slot_count(source)};
                std::memcpy(static_cast<void*>(free), slots_of(source), slot_count(source) * sizeof(node));

                auto* const first = free;
                free += slot_count(source);

                for (auto* scan = first; scan != free; scan++) {
                    if (kind_of(*scan) == kind::string) {
                        if ((scan->meta & inline_bit) == 0) {
                            std::memcpy(text, scan->text, size_of(*scan));
                            scan->text = text;
                            scan->meta &= ~owner_bit;
                            text += size_of(*scan);
                        }
                    } else if (is_container(*scan)) {
                        const auto count = slot_count(*scan);
                        if (count != 0) {
                            std::memcpy(static_cast<void*>(free), slots_of(*scan), count * sizeof(node));
                        }

                        scan->slots = count == 0 ? nullptr : reinterpret_cast<json::value*>(free);
                        scan->meta &= ~(owner_bit | loose_bit | tethered_bit);
                        free += count;
                    }
                }

                space.owner->slots = total.slots;
                space.owner->text = total.text;

                return {
                    .meta = tag(kind_of(source), owner_bit, size_of(source)),
                    .slots = reinterpret_cast<json::value*>(first),
                };
            }

            auto release_tree(node& held) noexcept -> void {
                auto* slots = slots_of(held);
                const auto count = slot_count(held);

                if (!is_pristine(held)) {
                    for (auto i = 0uz; i < count; i++) {
                        release(slots[i]);
                    }
                }

                if (!is_owner(held)) {
                    return;
                }

                auto* owned = header_of(held).owned;
                if (is_loose(held)) {
                    deallocate(&header_of(held));
                }
                if (owned != nullptr) {
                    close_arena(*owned);
                }
            }

            auto covers(const node& whole, const node& other) noexcept -> bool {
                const auto* slots = slots_of(whole);
                const auto* others = slots_of(other);

                for (auto i = 0uz; i < size_of(whole); i++) {
                    const auto key = text_of(slots[i * 2]);
                    if (locate(whole, key) != i) {
                        continue;
                    }

                    const auto match = locate(other, key);
                    if (!match || !equal(slots[(i * 2) + 1], others[(*match * 2) + 1])) {
                        return false;
                    }
                }

                return true;
            }

            auto equal_members(const node& lhs, const node& rhs) noexcept -> bool {
                const auto* slots = slots_of(lhs);
                const auto* others = slots_of(rhs);

                auto aligned = size_of(lhs) == size_of(rhs);
                for (auto i = 0uz; aligned && i < size_of(lhs); i++) {
                    aligned = text_of(slots[i * 2]) == text_of(others[i * 2]) &&
                              equal(slots[(i * 2) + 1], others[(i * 2) + 1]);
                }

                return aligned || (covers(lhs, rhs) && covers(rhs, lhs));
            }
        } // namespace

        auto open_arena(std::size_t bytes) -> region {
            const auto total = (sizeof(chunk) + sizeof(arena) + bytes + 15) & ~15uz;
            auto* base = static_cast<char*>(allocate(total));
            auto* first = ::new (static_cast<void*>(base)) chunk{.next = nullptr};
            auto* owner = ::new (static_cast<void*>(base + sizeof(chunk)))
                arena{.chunks = first, .slots = 0, .text = 0, .dirty = false};

            return {.owner = owner, .low = base + sizeof(chunk) + sizeof(arena), .high = base + total};
        }

        auto extend_arena(arena& owner, std::size_t bytes) -> region {
            const auto total = (sizeof(chunk) + bytes + 15) & ~15uz;
            auto* base = static_cast<char*>(allocate(total));
            owner.chunks = ::new (static_cast<void*>(base)) chunk{.next = owner.chunks};

            return {.owner = &owner, .low = base + sizeof(chunk), .high = base + total};
        }

        auto close_arena(arena& owner) noexcept -> void {
            auto* link = owner.chunks;
            while (link != nullptr) {
                auto* next = link->next;
                deallocate(link);
                link = next;
            }
        }

        auto locate(const node& held, std::string_view key) noexcept -> std::optional<std::size_t> {
            const auto* slots = slots_of(held);

            for (auto i = size_of(held); i > 0; i--) {
                if (text_of(slots[(i - 1) * 2]) == key) {
                    return i - 1;
                }
            }

            return std::nullopt;
        }

        auto text_node(std::string_view text) -> node {
            if (text.size() <= inline_capacity) {
                node made{.meta = tag(kind::string, inline_bit, text.size()), .natural = 0};
                std::ranges::copy(text, reinterpret_cast<char*>(&made) + inline_offset);
                return made;
            }

            auto* owned = static_cast<char*>(allocate(text.size() + text_padding));
            std::ranges::fill_n(std::ranges::copy(text, owned).out, text_padding, '\0');
            return {.meta = tag(kind::string, owner_bit, text.size()), .text = owned};
        }

        auto clone(const node& held) -> node {
            if (kind_of(held) == kind::string) {
                if ((held.meta & inline_bit) != 0) {
                    return held;
                }

                return text_node(text_of(held));
            }

            return is_container(held) ? clone_tree(held) : held;
        }

        auto take(node& held) -> node {
            if (is_borrowed(held)) {
                return clone(held);
            }

            const auto taken = held;
            if (is_container(held)) {
                held = {.meta = tag(kind_of(held)), .natural = 0};
            } else if (kind_of(held) == kind::string && (held.meta & inline_bit) == 0) {
                held = {.meta = tag(kind::string, inline_bit), .natural = 0};
            }

            return taken;
        }

        auto release(node& held) noexcept -> void {
            if (kind_of(held) == kind::string) {
                if (is_owner(held)) {
                    deallocate(const_cast<char*>(held.text));
                }
            } else if (is_container(held) && (is_owner(held) || size_of(held) != 0)) {
                release_tree(held);
            }
        }

        auto touch(node& held) noexcept -> void {
            if (is_owner(held) && header_of(held).owned != nullptr) {
                header_of(held).owned->dirty = true;
            }
        }

        auto equal(const node& lhs, const node& rhs) noexcept -> bool {
            if (kind_of(lhs) != kind_of(rhs)) {
                return false;
            }

            switch (kind_of(lhs)) {
            case kind::null:
                return true;
            case kind::boolean:
                return lhs.natural == rhs.natural;
            case kind::integer:
                return lhs.natural == rhs.natural && ((lhs.meta ^ rhs.meta) & unsigned_bit) == 0;
            case kind::floating:
                return lhs.floating == rhs.floating;
            case kind::string:
                return text_of(lhs) == text_of(rhs);
            case kind::array: {
                const auto* slots = slots_of(lhs);
                const auto* others = slots_of(rhs);

                auto same = size_of(lhs) == size_of(rhs);
                for (auto i = 0uz; same && i < size_of(lhs); i++) {
                    same = equal(slots[i], others[i]);
                }

                return same;
            }
            case kind::object:
                return equal_members(lhs, rhs);
            }

            return false;
        }
    } // namespace detail

    array::array(std::initializer_list<json::value> items) : array() {
        if (items.size() == 0) {
            return;
        }

        auto* slots = detail::room(m_node, items.size());
        for (const auto& item : items) {
            slots[size()] = detail::clone(detail::access::peek(item));
            detail::resize(m_node, size() + 1);
        }
    }

    auto array::operator=(const array& other) -> array& {
        if (this != &other) {
            const auto made = detail::clone(other.m_node);
            detail::release(m_node);
            m_node = made;
        }

        return *this;
    }

    auto array::operator=(array&& other) noexcept -> array& {
        if (this != &other) {
            const auto taken = detail::take(other.m_node);
            detail::release(m_node);
            m_node = taken;
        }

        return *this;
    }

    auto array::push_back(json::value item) -> json::value& {
        detail::touch(m_node);

        auto* slots = detail::room(m_node, 1);
        const auto count = size();
        slots[count] = detail::take(detail::access::held(item));
        detail::resize(m_node, count + 1);

        return m_node.slots[count];
    }

    auto array::insert(std::size_t index, json::value item) -> json::value& {
        detail::touch(m_node);

        auto* slots = detail::room(m_node, 1);
        const auto count = size();
        std::memmove(static_cast<void*>(slots + index + 1), slots + index,
                     (count - index) * sizeof(detail::node));
        slots[index] = detail::take(detail::access::held(item));
        detail::resize(m_node, count + 1);

        return m_node.slots[index];
    }

    auto array::erase(std::size_t index) noexcept -> void {
        detail::touch(m_node);

        auto* slots = detail::slots_of(m_node);
        const auto count = size();
        detail::release(slots[index]);
        std::memmove(static_cast<void*>(slots + index), slots + index + 1,
                     (count - index - 1) * sizeof(detail::node));
        detail::resize(m_node, count - 1);
    }

    auto array::clear() noexcept -> void {
        detail::touch(m_node);

        auto* slots = detail::slots_of(m_node);
        for (auto i = 0uz; i < size(); i++) {
            detail::release(slots[i]);
        }

        detail::resize(m_node, 0);
    }

    auto array::reserve(std::size_t capacity) -> void {
        detail::touch(m_node);
        detail::reserve_slots(m_node, std::max(capacity, size()));
    }

    object::object(std::initializer_list<object::entry> entries) : object() {
        for (const auto& [key, value] : entries) {
            set(key, value);
        }
    }

    auto object::operator=(const object& other) -> object& {
        if (this != &other) {
            const auto made = detail::clone(other.m_node);
            detail::release(m_node);
            m_node = made;
        }

        return *this;
    }

    auto object::operator=(object&& other) noexcept -> object& {
        if (this != &other) {
            const auto taken = detail::take(other.m_node);
            detail::release(m_node);
            m_node = taken;
        }

        return *this;
    }

    auto object::find(std::string_view key) const noexcept
        -> std::optional<std::reference_wrapper<const json::value>> {
        const auto found = detail::locate(m_node, key);
        return found ? std::optional{std::cref(m_node.slots[(*found * 2) + 1])} : std::nullopt;
    }

    auto object::find(std::string_view key) noexcept -> std::optional<std::reference_wrapper<json::value>> {
        detail::touch(m_node);

        const auto found = detail::locate(m_node, key);
        return found ? std::optional{std::ref(m_node.slots[(*found * 2) + 1])} : std::nullopt;
    }

    auto object::set(std::string_view key, json::value item) -> json::value& {
        detail::touch(m_node);

        if (const auto found = detail::locate(m_node, key)) {
            auto& slot = detail::slots_of(m_node)[(*found * 2) + 1];
            detail::release(slot);
            slot = detail::take(detail::access::held(item));
            return m_node.slots[(*found * 2) + 1];
        }

        auto* slots = detail::room(m_node, 2);
        const auto count = size();
        slots[count * 2] = detail::text_node(key);
        slots[(count * 2) + 1] = detail::take(detail::access::held(item));
        detail::resize(m_node, count + 1);

        return m_node.slots[(count * 2) + 1];
    }

    auto object::remove(std::string_view key) noexcept -> bool {
        detail::touch(m_node);

        auto* slots = detail::slots_of(m_node);
        const auto count = size();

        for (auto i = 0uz; i < count; i++) {
            if (detail::text_of(slots[i * 2]) == key) {
                slots[(i * 2) + 1].meta |= detail::doomed_bit;
            }
        }

        auto kept = 0uz;
        for (auto i = 0uz; i < count; i++) {
            auto& name = slots[i * 2];
            auto& held = slots[(i * 2) + 1];

            if ((held.meta & detail::doomed_bit) != 0) {
                held.meta &= ~detail::doomed_bit;
                detail::release(name);
                detail::release(held);
                continue;
            }

            slots[kept * 2] = name;
            slots[(kept * 2) + 1] = held;
            kept++;
        }

        detail::resize(m_node, kept);
        return kept != count;
    }

    auto object::clear() noexcept -> void {
        detail::touch(m_node);

        auto* slots = detail::slots_of(m_node);
        for (auto i = 0uz; i < size() * 2; i++) {
            detail::release(slots[i]);
        }

        detail::resize(m_node, 0);
    }

    auto object::reserve(std::size_t capacity) -> void {
        detail::touch(m_node);
        detail::reserve_slots(m_node, std::max(capacity, size()) * 2);
    }

    auto value::operator=(const value& other) -> value& {
        if (this != &other) {
            *this = value{other};
        }

        return *this;
    }

    auto value::operator=(value&& other) noexcept -> value& {
        if (this != &other) {
            const auto taken = other._owns() ? detail::take(other._held()) : other.m_plain.node;
            if (_owns()) {
                detail::release(_held());
            }

            _hold(taken);
        }

        return *this;
    }
} // namespace rei::formats::json
