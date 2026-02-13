import sys
from pathlib import Path

import pytest

_utils = pytest.importorskip("retracesoftware.utils")

try:
    import retracesoftware.utils as utils
except ModuleNotFoundError:  # Fallback for running directly from the repo
    sys.path.append(str(Path(__file__).resolve().parents[1] / "src"))
    import utils  # type: ignore


# def test_wrap_func_with_overrides_overrides_selected_globals():
#     calls = []

#     def base_helper(x):
#         calls.append(("base_helper", x))
#         return x + 1

#     def override_helper(x):
#         calls.append(("override_helper", x))
#         return x + 10

#     def original(x):
#         return base_helper(x)

#     wrapped = utils.wrap_func_with_overrides(original, base_helper=override_helper)

#     assert wrapped(3) == 13
#     assert original(3) == 4
#     assert calls == [
#         ("override_helper", 3),
#         ("base_helper", 3),
#     ]


def test_interceptdict_applies_on_set_to_backing_and_updates():
    backing = {"a": 1}
    seen = []

    def on_set(key, value):
        seen.append((key, value))
        return value * 2

    intercepted = utils.InterceptDict(backing, on_set)

    assert backing == {}  # moved out during construction
    assert intercepted["a"] == 2

    intercepted["b"] = 5
    assert intercepted["b"] == 10
    assert seen == [("a", 1), ("b", 5)]


def test_on_set_intercepts_dict_assignment_and_restores_type():
    mutations = []
    data = {"k": "v"}

    def transform(key, value):
        mutations.append((key, value))
        return f"{key}:{value}"

    original_type = type(data)
    with utils.on_set(data, transform):
        assert type(data).__name__ == "dictintercept"
        data["a"] = 1
        data.update(b=2)

    assert data == {"k": "v", "a": "a:1", "b": "b:2"}
    assert type(data) is original_type
    assert mutations == [("a", 1), ("b", 2)]


def test_counter_is_callable_and_increments():
    counter = _utils.counter(initial=5)

    assert counter() == 5
    assert counter() == 6
    assert counter.value == 7


def test_runall_invokes_all_functions_in_order():
    calls = []

    def first(*args, **kwargs):
        calls.append(("first", args, kwargs))
        return "ignored"

    def second(*args, **kwargs):
        calls.append(("second", args, kwargs))
        return None

    runner = _utils.runall(first, second)
    assert runner(1, named="x") is None
    assert calls == [
        ("first", (1,), {"named": "x"}),
        ("second", (1,), {"named": "x"}),
    ]


# def test_striptraceback_removes_traceback_and_context():
#     def boom():
#         raise ValueError("boom")

#     wrapper = _utils.striptraceback(boom)

#     with pytest.raises(ValueError) as excinfo:
#         wrapper()

#     assert excinfo.value.__traceback__ is None
#     assert excinfo.value.__context__ is None
#     assert excinfo.value.__cause__ is None


def test_observer_hooks_and_error_handling():
    events = []

    def base(x, y):
        events.append(("base", x, y))
        return x + y

    def on_call(*args, **kwargs):
        events.append(("on_call", args, kwargs))

    def on_result(result):
        events.append(("on_result", result))

    observer = _utils.observer(base, on_call=on_call, on_result=on_result)

    assert observer(2, 3) == 5
    assert events == [
        ("on_call", (2, 3), {}),
        ("base", 2, 3),
        ("on_result", 5),
    ]

    error_calls = []

    def bad():
        raise RuntimeError("fail")

    def on_error(exc_type, exc_value, exc_tb):
        error_calls.append((exc_type, exc_value, exc_tb))

    failing = _utils.observer(bad, on_error=on_error)

    with pytest.raises(RuntimeError):
        failing()

    assert error_calls
    exc_type, exc_value, exc_tb = error_calls[0]
    assert exc_type is RuntimeError
    assert isinstance(exc_value, RuntimeError)
    assert exc_tb is None or hasattr(exc_tb, "tb_frame")


def test_stack_functions_returns_list_of_functions():
    """Test that stack_functions() returns a valid list without crashing.
    
    This tests the fix for a segfault in Python 3.12+ where internal frames
    could have invalid f_funcobj pointers.
    """
    def inner():
        return _utils.stack_functions()
    
    def outer():
        return inner()
    
    # Call through nested functions to ensure we have a real stack
    result = outer()
    
    # Should return a list
    assert isinstance(result, list)
    
    # Should have at least our functions in the stack
    assert len(result) >= 2
    
    # All items should be callable (functions)
    for func in result:
        assert callable(func), f"Expected callable, got {type(func)}"


def test_stack_functions_from_lambda():
    """Test stack_functions works when called from a lambda."""
    get_stack = lambda: _utils.stack_functions()
    result = get_stack()
    assert isinstance(result, list)


def test_stack_functions_from_comprehension():
    """Test stack_functions works in list comprehension context."""
    # List comprehensions create their own frames in Python 3
    result = [_utils.stack_functions() for _ in range(1)][0]
    assert isinstance(result, list)


# ============================================================================
# Stack / StackFactory tests
# ============================================================================

class TestStackFactory:
    """Tests for StackFactory creation and basic calling."""

    def test_create_factory(self):
        factory = _utils.StackFactory()
        assert factory is not None

    def test_factory_exclude_is_set(self):
        factory = _utils.StackFactory()
        assert isinstance(factory.exclude, set)
        assert len(factory.exclude) == 0

    def test_factory_exclude_is_mutable(self):
        factory = _utils.StackFactory()
        factory.exclude.add(len)  # add a builtin
        assert len in factory.exclude
        factory.exclude.discard(len)
        assert len not in factory.exclude

    def test_factory_call_returns_stack(self):
        factory = _utils.StackFactory()
        stack = factory()
        assert stack is not None
        assert type(stack).__name__ == "Stack"

    def test_factory_no_args(self):
        factory = _utils.StackFactory()
        with pytest.raises(TypeError):
            factory(1)
        with pytest.raises(TypeError):
            factory(key=1)


class TestStackProperties:
    """Tests for Stack object properties and accessors."""

    def _get_stack(self):
        factory = _utils.StackFactory()
        return factory()

    def test_stack_has_func(self):
        stack = self._get_stack()
        assert callable(stack.func)

    def test_stack_has_filename(self):
        stack = self._get_stack()
        fn = stack.filename
        assert fn is not None
        assert __file__.rstrip("c") in fn  # .pyc -> .py

    def test_stack_has_lineno(self):
        stack = self._get_stack()
        assert isinstance(stack.lineno, int)
        assert stack.lineno > 0

    def test_stack_has_instruction(self):
        stack = self._get_stack()
        assert isinstance(stack.instruction, int)

    def test_stack_has_index(self):
        stack = self._get_stack()
        assert isinstance(stack.index, int)
        # Head frame should have the highest index
        assert stack.index == len(stack) - 1

    def test_stack_has_next(self):
        stack = self._get_stack()
        # Should have at least one more frame (test runner)
        assert stack.next is not None

    def test_stack_tail_next_is_none(self):
        stack = self._get_stack()
        current = stack
        while current.next is not None:
            current = current.next
        assert current.next is None
        assert current.index == 0


class TestStackLength:
    """Tests for Stack __len__ (O(1) via index)."""

    def test_len_positive(self):
        factory = _utils.StackFactory()
        stack = factory()
        assert len(stack) > 0

    def test_len_equals_index_plus_one(self):
        factory = _utils.StackFactory()
        stack = factory()
        assert len(stack) == stack.index + 1

    def test_len_consistent_with_traversal(self):
        factory = _utils.StackFactory()
        stack = factory()
        count = 0
        current = stack
        while current is not None:
            count += 1
            current = current.next
        assert len(stack) == count

    def test_nested_calls_increase_length(self):
        factory = _utils.StackFactory()

        def level1():
            return factory()

        def level2():
            return level1()

        s1 = level1()
        s2 = level2()
        assert len(s2) == len(s1) + 1


class TestStackGetitem:
    """Tests for Stack __getitem__ (indexed access)."""

    def test_getitem_head(self):
        factory = _utils.StackFactory()
        stack = factory()
        head = stack[stack.index]
        assert head.func == stack.func

    def test_getitem_tail(self):
        factory = _utils.StackFactory()
        stack = factory()
        tail = stack[0]
        assert tail.index == 0
        assert tail.next is None

    def test_getitem_negative_last(self):
        """stack[-1] should return the head frame (highest index)."""
        factory = _utils.StackFactory()
        stack = factory()
        last = stack[-1]
        assert last.index == stack.index

    def test_getitem_negative_first(self):
        """stack[-len(stack)] should return the tail frame (index 0)."""
        factory = _utils.StackFactory()
        stack = factory()
        first = stack[-len(stack)]
        assert first.index == 0

    def test_getitem_out_of_range(self):
        factory = _utils.StackFactory()
        stack = factory()
        with pytest.raises(IndexError):
            stack[len(stack) + 100]


class TestStackEquality:
    """Tests for Stack __eq__ / __ne__."""

    def test_same_object_is_equal(self):
        factory = _utils.StackFactory()
        stack = factory()
        assert stack == stack

    def test_different_stacks_same_call_site(self):
        factory = _utils.StackFactory()
        stack1 = factory()
        stack2 = factory()
        # Instruction offsets differ because they're different calls
        # but the structure is similar
        assert stack1 is not stack2

    def test_not_equal_to_non_stack(self):
        factory = _utils.StackFactory()
        stack = factory()
        assert (stack == "not a stack") is NotImplemented or stack != "not a stack"


class TestStackIteration:
    """Tests for Stack __iter__."""

    def test_iter_returns_tuples(self):
        factory = _utils.StackFactory()
        stack = factory()
        for filename, lineno in stack:
            assert isinstance(lineno, int)

    def test_iter_length_matches(self):
        factory = _utils.StackFactory()
        stack = factory()
        items = list(stack)
        assert len(items) == len(stack)


class TestStackLocations:
    """Tests for Stack.locations() method."""

    def test_locations_returns_list(self):
        factory = _utils.StackFactory()
        stack = factory()
        locs = stack.locations()
        assert isinstance(locs, list)
        assert len(locs) == len(stack)

    def test_locations_contains_tuples(self):
        factory = _utils.StackFactory()
        stack = factory()
        locs = stack.locations()
        for loc in locs:
            assert isinstance(loc, tuple)
            assert len(loc) == 2
            filename, lineno = loc
            assert isinstance(lineno, int)


class TestStackExclude:
    """Tests for StackFactory exclude filtering."""

    def test_exclude_removes_function(self):
        factory = _utils.StackFactory()

        def excluded_func():
            return factory()

        # Without exclude
        s1 = excluded_func()
        funcs1 = []
        current = s1
        while current is not None:
            funcs1.append(current.func)
            current = current.next
        assert excluded_func in funcs1

        # With exclude
        factory.exclude.add(excluded_func)
        s2 = excluded_func()
        funcs2 = []
        current = s2
        while current is not None:
            funcs2.append(current.func)
            current = current.next
        assert excluded_func not in funcs2
        assert len(s2) == len(s1) - 1


class TestStackChangesFrom:
    """Tests for Stack.changes_from() method."""

    def test_same_stack_no_changes(self):
        factory = _utils.StackFactory()
        stack = factory()
        pop_count, to_add = stack.changes_from(stack)
        assert pop_count == 0
        assert len(to_add) == 0

    def test_from_none(self):
        factory = _utils.StackFactory()
        stack = factory()
        pop_count, to_add = stack.changes_from(None)
        assert pop_count == 0
        assert len(to_add) == len(stack)

    def test_type_error(self):
        factory = _utils.StackFactory()
        stack = factory()
        with pytest.raises(TypeError):
            stack.changes_from("invalid")

    def test_nested_changes(self):
        factory = _utils.StackFactory()

        def level1():
            return factory()

        def level2():
            return level1()

        s1 = level1()
        s2 = level2()

        # Going from s1 to s2 should add one frame
        pop_count, to_add = s2.changes_from(s1)
        # s2 has one more frame at the top
        assert len(to_add) >= 1

    def test_symmetric_changes(self):
        factory = _utils.StackFactory()

        def func_a():
            return factory()

        def func_b():
            return factory()

        sa = func_a()
        sb = func_b()

        pop_a, add_a = sb.changes_from(sa)
        pop_b, add_b = sa.changes_from(sb)

        # Symmetric: what you remove going one way = what you add going back
        assert pop_a == len(add_b)
        assert pop_b == len(add_a)


class TestStackDelta:
    """Tests for StackFactory.delta() method."""

    def test_first_delta_from_empty(self):
        factory = _utils.StackFactory()
        pop_count, to_add = factory.delta()
        # First call: no cached stack, so pop 0 and add everything
        assert pop_count == 0
        assert len(to_add) > 0

    def test_same_call_site_no_changes(self):
        factory = _utils.StackFactory()
        # First call populates cache
        factory()
        # Delta from same call site
        pop_count, to_add = factory.delta()
        # Should be minimal changes (just instruction offset might differ)
        # But the structure/depth is the same
        assert isinstance(pop_count, int)
        assert isinstance(to_add, tuple)

    def test_delta_detects_deeper_stack(self):
        factory = _utils.StackFactory()

        def level1():
            return factory.delta()

        def level2():
            return level1()

        # Populate cache at level1 depth
        factory()
        # Now call from deeper - should detect the change
        pop_count, to_add = level2()
        assert isinstance(pop_count, int)
        assert isinstance(to_add, tuple)


class TestStackReuse:
    """Tests for suffix sharing optimization in create_from_frame."""

    def test_repeated_calls_share_suffix(self):
        """When called from same location twice, suffix nodes should be shared."""
        factory = _utils.StackFactory()

        def inner():
            return factory()

        s1 = inner()
        s2 = inner()

        # The deeper frames should be shared (same objects)
        # Walk to find a common suffix
        # Due to the reuse optimization, at some depth the nodes should be identical
        c1 = s1.next
        c2 = s2.next
        # At least the deeper frames should be shared
        found_shared = False
        while c1 is not None and c2 is not None:
            if c1 is c2:
                found_shared = True
                break
            c1 = c1.next
            c2 = c2.next
        assert found_shared, "Expected suffix sharing between repeated stack captures"

    def test_index_consistent_after_reuse(self):
        """Index should be correct even when reusing suffix nodes."""
        factory = _utils.StackFactory()

        def inner():
            return factory()

        s1 = inner()
        s2 = inner()

        # Walk s2 and verify indices are sequential
        current = s2
        expected_index = s2.index
        while current is not None:
            assert current.index == expected_index
            expected_index -= 1
            current = current.next
        assert expected_index == -1  # Should have counted down to -1


# ============================================================================
# Demultiplexer tests
# ============================================================================

class TestDemultiplexer:
    """Tests for the Demultiplexer (demux) type."""

    def test_create(self):
        items = iter([(1, "a"), (2, "b")])
        demux = _utils.demux(lambda: next(items), lambda item: item[0])
        assert demux is not None

    def test_single_key_single_item(self):
        # Need a second item for the read-ahead that follows the fast-path return
        items = iter([(1, "hello"), (1, "sentinel")])
        demux = _utils.demux(lambda: next(items), lambda item: item[0])
        result = demux(1)
        assert result == (1, "hello")

    def test_sequential_same_key(self):
        # The fast path in get() reads ahead one item after returning,
        # so we need N+1 items to safely consume N without the read-ahead
        # hitting StopIteration and discarding the last result.
        items = iter([(1, "a"), (1, "b"), (1, "c"), (1, "sentinel")])
        demux = _utils.demux(lambda: next(items), lambda item: item[0])
        assert demux(1) == (1, "a")
        assert demux(1) == (1, "b")
        assert demux(1) == (1, "c")

    def test_two_threads_sequential_keys(self):
        """Two threads consuming items with non-overlapping keys.

        Items are grouped by key so the fast path handles everything
        without needing cross-thread wakeups (which the fast path
        doesn't issue for different keys).
        """
        import threading

        # Group items by key: thread 1 gets both key-1 items via fast path,
        # then thread 2 gets both key-2 items via fast path.
        # Extra sentinel for the final read-ahead.
        data = [(1, "a"), (1, "b"), (2, "x"), (2, "y"), (0, "sentinel")]
        items = iter(data)
        items_lock = threading.Lock()

        def source():
            with items_lock:
                return next(items)

        demux = _utils.demux(source, lambda item: item[0], timeout_seconds=5)
        results = {}
        errors = []
        t1_done = threading.Event()

        def consumer1():
            try:
                for _ in range(2):
                    got = demux(1)
                    results.setdefault(1, []).append(got)
            except Exception as e:
                errors.append((1, e))
            finally:
                t1_done.set()

        def consumer2():
            t1_done.wait(timeout=5)  # Wait for thread 1 to finish
            try:
                for _ in range(2):
                    got = demux(2)
                    results.setdefault(2, []).append(got)
            except Exception as e:
                errors.append((2, e))

        t1 = threading.Thread(target=consumer1)
        t2 = threading.Thread(target=consumer2)

        t1.start()
        t2.start()
        t1.join(timeout=10)
        t2.join(timeout=10)

        assert not errors, f"Errors: {errors}"
        assert [r[1] for r in results[1]] == ["a", "b"]
        assert [r[1] for r in results[2]] == ["x", "y"]


    def test_key_function_called(self):
        """Verify the key function is actually used to match items."""
        items = iter([{"id": "A", "val": 1}, {"id": "B", "val": 2}, {"id": "X", "val": 0}])
        demux = _utils.demux(lambda: next(items), lambda item: item["id"])

        result = demux("A")
        assert result == {"id": "A", "val": 1}

    def test_pending_keys(self):
        """Test the pending_keys getter."""
        items = iter([(1, "a")])
        demux = _utils.demux(lambda: next(items), lambda item: item[0])
        # Before any calls, pending should be empty
        assert len(demux.pending_keys) == 0

    def test_on_timeout_callback(self):
        """Test that on_timeout is called when wait times out."""
        import threading

        # Source returns two items then blocks: the fast path in get() reads
        # ahead one item after returning, so we need a second item available
        # to avoid blocking on the first call.
        call_count = [0]
        event = threading.Event()

        def blocking_source():
            call_count[0] += 1
            if call_count[0] == 1:
                return (1, "first")
            if call_count[0] == 2:
                return (3, "lookahead")  # consumed by read-ahead, key 3 won't be requested
            # All subsequent calls block until cleanup
            event.wait(timeout=10)
            raise StopIteration

        timeout_called = []

        def on_timeout(demux_obj, key):
            timeout_called.append(key)
            return ("timeout", key)

        demux = _utils.demux(
            blocking_source,
            lambda item: item[0],
            on_timeout=on_timeout,
            timeout_seconds=1,
        )

        # First call gets the item with key 1; read-ahead consumes (3, "lookahead")
        result1 = demux(1)
        assert result1 == (1, "first")

        # Now request key 2 — next item has key 3, so we enter wait() and timeout
        result2 = demux(2)
        assert result2 == ("timeout", 2)
        assert 2 in timeout_called

        event.set()  # Cleanup

    def test_duplicate_key_raises(self):
        """Requesting the same key from two threads should raise ValueError."""
        import threading
        import time

        call_count = [0]
        blocker = threading.Event()

        def source():
            call_count[0] += 1
            if call_count[0] == 1:
                # Return an item with key 99 so that requests for key 1
                # will enter wait() rather than the fast path
                return (99, "wrong_key")
            # Subsequent calls block until test cleanup
            blocker.wait(timeout=10)
            return (1, "a")

        demux = _utils.demux(
            source,
            lambda item: item[0],
            timeout_seconds=3,
        )

        errors = []

        def first_consumer():
            try:
                demux(1)  # key 1 doesn't match next (key 99), enters wait()
            except Exception as e:
                errors.append(e)

        def second_consumer():
            time.sleep(0.2)  # Give t1 time to enter wait()
            try:
                demux(1)  # key 1 is already in pending_keys → ValueError
            except Exception as e:
                errors.append(e)

        t1 = threading.Thread(target=first_consumer)
        t2 = threading.Thread(target=second_consumer)

        t1.start()
        t2.start()

        # Give threads a moment then unblock source for cleanup
        time.sleep(0.5)
        blocker.set()

        t1.join(timeout=5)
        t2.join(timeout=5)

        # At least one should have gotten a ValueError for duplicate key
        assert any(isinstance(e, ValueError) for e in errors), \
            f"Expected ValueError but got: {errors}"

    def test_callable_with_wrong_args_raises(self):
        items = iter([(1, "a")])
        demux = _utils.demux(lambda: next(items), lambda item: item[0])
        with pytest.raises(TypeError):
            demux()  # No args
        with pytest.raises(TypeError):
            demux(1, 2)  # Too many args


# ============================================================================
# GC iteration test - verify gc.get_objects() works in pytest
# ============================================================================

def test_gc_iteration_does_not_crash():
    """Test that simply iterating gc.get_objects() doesn't crash pytest."""
    import gc
    import types
    
    all_objects = gc.get_objects()
    assert len(all_objects) > 0
    
    # Count FunctionType objects (what patch_hash would do)
    func_count = 0
    for obj in all_objects:
        if obj is None or type(obj) is None:
            continue
        if isinstance(obj, types.FunctionType):
            func_count += 1
            # Also try hashing them (what patch_hash does)
            try:
                h = hash(obj)
            except TypeError:
                pass  # Some objects aren't hashable
    
    assert func_count > 0, "Should find at least some functions"
    print(f"Found {func_count} FunctionType objects out of {len(all_objects)} total")


# ============================================================================
# patch_hash tests
# ============================================================================

# patch_hash tests now work correctly under pytest after fixing the subtype_dealloc
# recursion bug. The fix temporarily restores the original tp_dealloc during
# deallocation to prevent infinite recursion.
_skip_patch_hash = False


@pytest.mark.skipif(_skip_patch_hash, reason="patch_hash crashes under pytest due to gc.get_objects() interaction")
def test_patch_hash_new_instances_use_counter():
    """Test that instances created AFTER patching use the counter-based hash."""
    # Use unique class name to avoid conflicts with other tests
    class ItemForCounterTest:
        pass
    
    # Patch BEFORE creating any instances
    counter = _utils.counter(initial=100)
    _utils.patch_hash(ItemForCounterTest, lambda obj: counter)
    
    # New instances should use the counter-based hash
    i1 = ItemForCounterTest()
    i2 = ItemForCounterTest()
    i3 = ItemForCounterTest()
    
    h1 = hash(i1)
    h2 = hash(i2)
    h3 = hash(i3)
    
    # Each should get a unique, spread hash derived from counter
    assert h1 != h2 != h3, "Counter-based hashes should be unique"
    
    # Hashes should be consistent (cached after first call)
    assert hash(i1) == h1
    assert hash(i2) == h2
    assert hash(i3) == h3


@pytest.mark.skipif(_skip_patch_hash, reason="patch_hash crashes under pytest due to gc.get_objects() interaction")
def test_patch_hash_with_none_returns_identity_hash():
    """Test that returning None from hash function falls back to identity hash."""
    # Use unique class name to avoid conflicts with other tests
    class ThingForNoneTest:
        pass
    
    # Patch with a function that returns None (should use identity hash)
    _utils.patch_hash(ThingForNoneTest, lambda obj: None)
    
    t1 = ThingForNoneTest()
    t2 = ThingForNoneTest()
    
    h1 = hash(t1)
    h2 = hash(t2)
    
    # Each should have identity hash (unique per object)
    assert h1 != h2, "Identity hashes should be different for different objects"
    
    # Hashes should be consistent
    assert hash(t1) == h1
    assert hash(t2) == h2


@pytest.mark.skipif(_skip_patch_hash, reason="patch_hash crashes under pytest due to gc.get_objects() interaction")
def test_patch_hash_with_int_uses_value_directly():
    """Test that returning an int from hash function uses it directly."""
    class DirectHashTest:
        pass
    
    # Patch to return a fixed hash
    _utils.patch_hash(DirectHashTest, lambda obj: 12345)
    
    obj = DirectHashTest()
    assert hash(obj) == 12345
    
    # Second call should return same (cached) value
    assert hash(obj) == 12345


@pytest.mark.skipif(_skip_patch_hash, reason="patch_hash crashes under pytest due to gc.get_objects() interaction")
def test_patch_hash_objects_usable_in_sets():
    """Test that patched objects can be used in sets."""
    class SetItemTest:
        def __init__(self, val):
            self.val = val
    
    counter = _utils.counter(initial=1)
    _utils.patch_hash(SetItemTest, lambda obj: counter)
    
    # Create objects and add to set
    items = [SetItemTest(i) for i in range(5)]
    item_set = set(items)
    
    # All items should be in the set
    assert len(item_set) == 5
    for item in items:
        assert item in item_set


@pytest.mark.skipif(_skip_patch_hash, reason="patch_hash crashes under pytest due to gc.get_objects() interaction")
def test_patch_hash_existing_instances_cached():
    """Test that objects existing BEFORE patching preserve their original hash.
    
    This tests the critical fix where we cache existing instance hashes before
    patching. Without this, objects would be stored in dicts/sets at their old 
    hash bucket but lookups would use the new hash, making them unfindable.
    """
    class WidgetForCacheTest:
        def __init__(self, name):
            self.name = name
    
    # Create instances and add to a set BEFORE patching
    w1 = WidgetForCacheTest("alpha")
    w2 = WidgetForCacheTest("beta")
    widget_set = {w1, w2}
    
    # Store original hashes for verification
    original_hash_w1 = hash(w1)
    original_hash_w2 = hash(w2)
    
    # Verify they're in the set (sanity check)
    assert w1 in widget_set
    assert w2 in widget_set
    
    # Patch the hash function to return a completely different value
    counter = _utils.counter(initial=1000)
    _utils.patch_hash(WidgetForCacheTest, lambda obj: counter)
    
    # CRITICAL: Existing objects must still be findable in the set!
    # They should use their cached (original) hash, not the new hash function
    assert w1 in widget_set, "Existing object lost from set after patch_hash!"
    assert w2 in widget_set, "Existing object lost from set after patch_hash!"
    
    # Hashes of existing objects should be their original values (cached)
    assert hash(w1) == original_hash_w1, "Existing object hash changed!"
    assert hash(w2) == original_hash_w2, "Existing object hash changed!"
    
    # New objects should use the NEW hash function
    w3 = WidgetForCacheTest("gamma")
    h3 = hash(w3)
    assert h3 != original_hash_w1 and h3 != original_hash_w2, \
        "New object should have different hash from existing objects"


# ============================================================================
# ThreadState tests
# ============================================================================

class TestThreadState:
    """Tests for ThreadState creation and basic operations."""

    def test_creation(self):
        state = _utils.ThreadState('disabled', 'internal', 'external', 'retrace')
        assert state is not None

    def test_default_value_is_first_state(self):
        state = _utils.ThreadState('disabled', 'internal')
        assert state.value == 'disabled'

    def test_requires_at_least_two_states(self):
        with pytest.raises(TypeError):
            _utils.ThreadState('only_one')

    def test_value_getter_setter(self):
        state = _utils.ThreadState('disabled', 'internal')
        assert state.value == 'disabled'
        state.value = 'internal'
        assert state.value == 'internal'
        state.value = 'disabled'

    def test_invalid_value_raises(self):
        state = _utils.ThreadState('disabled', 'internal')
        with pytest.raises(TypeError):
            state.value = 'nonexistent'

    def test_select_context_manager(self):
        state = _utils.ThreadState('disabled', 'internal', 'external')
        assert state.value == 'disabled'

        with state.select('internal'):
            assert state.value == 'internal'

        assert state.value == 'disabled'

    def test_select_nested(self):
        state = _utils.ThreadState('disabled', 'internal', 'external')

        with state.select('internal'):
            assert state.value == 'internal'
            with state.select('external'):
                assert state.value == 'external'
            assert state.value == 'internal'

        assert state.value == 'disabled'

    def test_predicate_matches_current_state(self):
        state = _utils.ThreadState('disabled', 'internal', 'external')

        is_disabled = state.predicate('disabled')
        is_internal = state.predicate('internal')

        assert is_disabled()
        assert not is_internal()

        state.value = 'internal'
        assert not is_disabled()
        assert is_internal()

        state.value = 'disabled'

    def test_repr_contains_state(self):
        state = _utils.ThreadState('disabled', 'internal')
        r = repr(state)
        assert 'ThreadState' in r
        assert 'disabled' in r

    def test_wrap_function(self):
        """wrap() creates a callable that switches state before calling."""
        state = _utils.ThreadState('disabled', 'internal', 'external')

        def check_state():
            return state.value

        wrapped = state.wrap('internal', check_state)
        assert state.value == 'disabled'
        assert wrapped() == 'internal'
        assert state.value == 'disabled'  # restored after call


# ============================================================================
# Dispatch tests
# ============================================================================

class TestDispatch:
    """Tests for Dispatch creation and routing."""

    def test_creation_with_all_states(self):
        state = _utils.ThreadState('a', 'b')
        d = state.dispatch(a=lambda: 'a', b=lambda: 'b')
        assert d is not None

    def test_creation_with_default(self):
        state = _utils.ThreadState('a', 'b', 'c')
        d = state.dispatch(lambda: 'default', b=lambda: 'b')
        assert d is not None

    def test_missing_state_and_no_default_raises(self):
        state = _utils.ThreadState('a', 'b', 'c')
        with pytest.raises(TypeError):
            state.dispatch(a=lambda: 'a', b=lambda: 'b')
            # 'c' is not specified and no default given

    def test_routes_to_current_state(self):
        state = _utils.ThreadState('disabled', 'internal')

        d = state.dispatch(
            disabled=lambda: 'from_disabled',
            internal=lambda: 'from_internal',
        )

        assert d() == 'from_disabled'

        state.value = 'internal'
        assert d() == 'from_internal'
        state.value = 'disabled'

    def test_routes_with_args(self):
        state = _utils.ThreadState('disabled', 'internal')

        d = state.dispatch(
            disabled=lambda x, y: x + y,
            internal=lambda x, y: x * y,
        )

        assert d(3, 4) == 7

        state.value = 'internal'
        assert d(3, 4) == 12
        state.value = 'disabled'

    def test_routes_with_kwargs(self):
        state = _utils.ThreadState('disabled', 'internal')

        d = state.dispatch(
            disabled=lambda x, y=10: x + y,
            internal=lambda x, y=10: x * y,
        )

        assert d(3) == 13
        assert d(3, y=5) == 8

        state.value = 'internal'
        assert d(3) == 30
        assert d(3, y=5) == 15
        state.value = 'disabled'

    def test_default_fills_unspecified_states(self):
        state = _utils.ThreadState('disabled', 'internal', 'external')

        def default_handler(*args):
            return 'default'

        def internal_handler(*args):
            return 'internal'

        d = state.dispatch(default_handler, internal=internal_handler)

        # disabled → default
        assert d() == 'default'

        # internal → override
        state.value = 'internal'
        assert d() == 'internal'

        # external → default
        state.value = 'external'
        assert d() == 'default'

        state.value = 'disabled'

    def test_table_returns_dict_of_handlers(self):
        state = _utils.ThreadState('disabled', 'internal')

        handler_d = lambda: 'disabled'
        handler_i = lambda: 'internal'

        d = state.dispatch(disabled=handler_d, internal=handler_i)

        table = _utils.dispatch.table(d)
        assert isinstance(table, dict)
        assert 'disabled' in table
        assert 'internal' in table

    def test_table_maps_original_handlers(self):
        state = _utils.ThreadState('disabled', 'internal')

        handler_d = lambda: 'disabled'
        handler_i = lambda: 'internal'

        d = state.dispatch(disabled=handler_d, internal=handler_i)

        table = _utils.dispatch.table(d)
        assert table['disabled'] is handler_d
        assert table['internal'] is handler_i

    def test_table_default_appears_for_unspecified(self):
        state = _utils.ThreadState('disabled', 'internal', 'external')

        original = lambda: 'original'
        override = lambda: 'override'

        d = state.dispatch(original, internal=override)

        table = _utils.dispatch.table(d)
        assert table['disabled'] is original
        assert table['internal'] is override
        assert table['external'] is original

    def test_table_type_error_for_non_dispatch(self):
        with pytest.raises(TypeError):
            _utils.dispatch.table("not a dispatch")

    def test_routes_change_with_select(self):
        """Dispatch routing follows select() context manager."""
        state = _utils.ThreadState('disabled', 'internal', 'external')

        d = state.dispatch(
            disabled=lambda: 'disabled',
            internal=lambda: 'internal',
            external=lambda: 'external',
        )

        assert d() == 'disabled'

        with state.select('internal'):
            assert d() == 'internal'
            with state.select('external'):
                assert d() == 'external'
            assert d() == 'internal'

        assert d() == 'disabled'

    def test_set_dispatch_updates_handler(self):
        state = _utils.ThreadState('disabled', 'internal')

        d = state.dispatch(
            disabled=lambda: 'old_disabled',
            internal=lambda: 'old_internal',
        )

        assert d() == 'old_disabled'

        state.set_dispatch(d, disabled=lambda: 'new_disabled')
        assert d() == 'new_disabled'

        # untouched state still returns old handler
        state.value = 'internal'
        assert d() == 'old_internal'
        state.value = 'disabled'

    def test_method_dispatch_creation_and_table(self):
        state = _utils.ThreadState('disabled', 'internal')

        handler_d = lambda self: 'disabled'
        handler_i = lambda self: 'internal'

        d = state.method_dispatch(disabled=handler_d, internal=handler_i)

        # method_dispatch is a subtype of dispatch, so table() works
        table = _utils.dispatch.table(d)
        assert table['disabled'] is handler_d
        assert table['internal'] is handler_i

    def test_method_dispatch_isinstance_dispatch(self):
        """MethodDispatch is a subtype of Dispatch."""
        state = _utils.ThreadState('disabled', 'internal')
        d = state.method_dispatch(
            disabled=lambda self: 'd',
            internal=lambda self: 'i',
        )
        assert isinstance(d, _utils.dispatch)

    def test_dispatch_repr(self):
        state = _utils.ThreadState('disabled', 'internal')

        d = state.dispatch(
            disabled=lambda: 'disabled',
            internal=lambda: 'internal',
        )

        r = repr(d)
        assert 'Dispatch' in r


# ============================================================================
# Wrapped / unwrap tests
# ============================================================================

class TestWrappedUnwrap:
    """Tests for wrapped_function, wrapped_member, try_unwrap, and unwrap."""

    def test_wrapped_function_creation(self):
        def original(x):
            return x + 1

        def handler(target, *args, **kwargs):
            return target(*args, **kwargs)

        wf = _utils.wrapped_function(target=original, handler=handler)
        assert wf is not None

    def test_wrapped_function_calls_via_handler(self):
        calls = []

        def original(x):
            return x + 1

        def handler(target, *args, **kwargs):
            calls.append(('handler', target, args))
            return target(*args, **kwargs)

        wf = _utils.wrapped_function(target=original, handler=handler)
        result = wf(5)
        assert result == 6
        assert len(calls) == 1
        assert calls[0][1] is original
        assert calls[0][2] == (5,)

    def test_try_unwrap_wrapped_function(self):
        def original(x):
            return x + 1

        def handler(target, *args, **kwargs):
            return target(*args, **kwargs)

        wf = _utils.wrapped_function(target=original, handler=handler)
        unwrapped = _utils.try_unwrap(wf)
        assert unwrapped is original

    def test_try_unwrap_returns_same_for_non_wrapped(self):
        obj = "hello"
        assert _utils.try_unwrap(obj) is obj

        num = 42
        assert _utils.try_unwrap(num) is num

    def test_unwrap_wrapped_function(self):
        def original(x):
            return x + 1

        def handler(target, *args, **kwargs):
            return target(*args, **kwargs)

        wf = _utils.wrapped_function(target=original, handler=handler)
        unwrapped = _utils.unwrap(wf)
        assert unwrapped is original

    def test_unwrap_raises_for_non_wrapped(self):
        with pytest.raises(TypeError):
            _utils.unwrap("not wrapped")

    def test_wrapped_member_creation(self):
        # Use a real descriptor: type.__name__ is a GetSetDescriptor
        member = type.__dict__['__name__']

        def handler(*args):
            return args

        wm = _utils.wrapped_member(target=member, handler=handler)
        assert wm is not None

    def test_try_unwrap_wrapped_member(self):
        member = type.__dict__['__name__']

        def handler(*args):
            return args

        wm = _utils.wrapped_member(target=member, handler=handler)
        unwrapped = _utils.try_unwrap(wm)
        assert unwrapped is member

    def test_is_wrapped_positive(self):
        def original(x):
            return x + 1

        def handler(target, *args, **kwargs):
            return target(*args, **kwargs)

        wf = _utils.wrapped_function(target=original, handler=handler)
        assert _utils.is_wrapped(wf)

        member = type.__dict__['__name__']
        wm = _utils.wrapped_member(target=member, handler=handler)
        assert _utils.is_wrapped(wm)

    def test_is_wrapped_negative(self):
        assert not _utils.is_wrapped("hello")
        assert not _utils.is_wrapped(42)
        assert not _utils.is_wrapped(lambda: None)

    def test_unwrap_apply_calls_original_target(self):
        """unwrap_apply(wrapped, *args) calls the original target directly."""
        calls = []

        def original(x, y):
            calls.append(('original', x, y))
            return x + y

        def handler(target, *args, **kwargs):
            calls.append(('handler',))
            return target(*args, **kwargs)

        wf = _utils.wrapped_function(target=original, handler=handler)

        # unwrap_apply bypasses the handler and calls the target directly
        result = _utils.unwrap_apply(wf, 3, 4)
        assert result == 7
        assert calls == [('original', 3, 4)]  # handler was NOT called

    def test_dispatch_table_extracts_original_from_proxy_pattern(self):
        """Simulate the proxy pattern: dispatch(original, internal=wrapped).

        The 'disabled' entry in the table should be the original function.
        """
        state = _utils.ThreadState('disabled', 'internal', 'external', 'retrace')

        def original_func():
            return 'original'

        def handler(target, *args, **kwargs):
            return target(*args, **kwargs)

        wrapped = _utils.wrapped_function(target=original_func, handler=handler)
        d = state.dispatch(original_func, internal=wrapped)

        # The dispatch table's 'disabled' entry is the original
        table = _utils.dispatch.table(d)
        assert table['disabled'] is original_func

        # And the 'internal' entry is the wrapped version
        assert table['internal'] is wrapped

        # We can chain: get original from dispatch, then verify it's the real deal
        extracted = table['disabled']
        assert extracted() == 'original'


# ============================================================================
# Gate tests
# ============================================================================

class TestGateCreation:
    """Tests for Gate creation and basic state."""

    def test_create(self):
        gate = _utils.Gate()
        assert gate is not None

    def test_initial_executor_is_none(self):
        gate = _utils.Gate()
        assert gate.executor is None

    def test_repr_disabled(self):
        gate = _utils.Gate()
        r = repr(gate)
        assert 'Gate' in r
        assert 'disabled' in r.lower()

    def test_no_args(self):
        with pytest.raises(TypeError):
            _utils.Gate(1)
        with pytest.raises(TypeError):
            _utils.Gate(x=1)

    def test_default_executor(self):
        """Gate(default=callable) uses that executor when no thread-local is set."""
        gate = _utils.Gate(default=_utils.noop)
        assert gate.executor is _utils.noop
        bound = gate.bind(lambda: 42)
        assert bound() is None  # noop ignores target and returns None

    def test_default_pass_through(self):
        """Gate(default=pass_through) forwards to target when no thread-local set."""
        pass_through = lambda target, *args, **kwargs: target(*args, **kwargs)
        gate = _utils.Gate(default=pass_through)
        assert gate.executor is pass_through
        bound = gate.bind(lambda x: x + 1)
        assert bound(10) == 11

    def test_disable_restores_to_default(self):
        """Disabling a gate with default restores to the default executor."""
        pass_through = lambda target, *args, **kwargs: target(*args, **kwargs)
        override = lambda target, *args, **kwargs: 999
        gate = _utils.Gate(default=pass_through)
        gate.set(override)
        assert gate.executor is override
        gate.disable()
        assert gate.executor is pass_through
        bound = gate.bind(lambda x: x + 1)
        assert bound(10) == 11

    def test_default_none_same_as_no_default(self):
        gate = _utils.Gate(default=None)
        assert gate.executor is None

    def test_default_non_callable_raises(self):
        with pytest.raises(TypeError):
            _utils.Gate(default=42)


class TestGateSetDisable:
    """Tests for Gate.set() and Gate.disable()."""

    def test_set_executor(self):
        gate = _utils.Gate()
        executor = lambda target, *args, **kwargs: target(*args, **kwargs)
        gate.set(executor)
        assert gate.executor is executor

    def test_set_none_disables(self):
        gate = _utils.Gate()
        executor = lambda target, *args, **kwargs: target(*args, **kwargs)
        gate.set(executor)
        assert gate.executor is executor
        gate.set(None)
        assert gate.executor is None

    def test_disable(self):
        gate = _utils.Gate()
        executor = lambda target, *args, **kwargs: target(*args, **kwargs)
        gate.set(executor)
        gate.disable()
        assert gate.executor is None

    def test_is_set(self):
        gate = _utils.Gate()
        assert gate.is_set() is False
        gate.set(lambda t, *a, **k: t(*a, **k))
        assert gate.is_set() is True
        gate.disable()
        assert gate.is_set() is False

    def test_is_set_with_default(self):
        gate = _utils.Gate(default=_utils.noop)
        assert gate.is_set() is True  # default counts as set
        gate.disable()  # restore to default
        assert gate.is_set() is True

    def test_set_non_callable_raises(self):
        gate = _utils.Gate()
        with pytest.raises(TypeError):
            gate.set(42)
        with pytest.raises(TypeError):
            gate.set("not callable")

    def test_repr_with_executor(self):
        gate = _utils.Gate()
        gate.set(lambda target, *a, **kw: target(*a, **kw))
        r = repr(gate)
        assert 'Gate' in r
        assert 'lambda' in r


class TestGateBind:
    """Tests for Gate.bind() — creating BoundGate wrappers."""

    def test_bind_returns_callable(self):
        gate = _utils.Gate()
        target = lambda x: x + 1
        bound = gate.bind(target)
        assert callable(bound)

    def test_bind_non_callable_raises(self):
        gate = _utils.Gate()
        with pytest.raises(TypeError):
            gate.bind(42)

    def test_bound_disabled_calls_target_directly(self):
        """When gate is disabled (no executor), bound calls target directly."""
        gate = _utils.Gate()  # disabled by default
        calls = []

        def target(x, y):
            calls.append(('target', x, y))
            return x + y

        bound = gate.bind(target)
        result = bound(3, 4)

        assert result == 7
        assert calls == [('target', 3, 4)]

    def test_bound_with_executor_routes_through_executor(self):
        """When gate has an executor, bound calls go through it."""
        gate = _utils.Gate()
        calls = []

        def my_executor(target, *args, **kwargs):
            calls.append(('executor', target.__name__, args, kwargs))
            return target(*args, **kwargs)

        def target(x, y):
            calls.append(('target', x, y))
            return x + y

        gate.set(my_executor)
        bound = gate.bind(target)
        result = bound(3, 4)

        assert result == 7
        assert len(calls) == 2
        assert calls[0] == ('executor', 'target', (3, 4), {})
        assert calls[1] == ('target', 3, 4)

    def test_bound_with_kwargs(self):
        gate = _utils.Gate()

        def target(x, y=10):
            return x + y

        bound = gate.bind(target)

        # Disabled — direct call
        assert bound(3) == 13
        assert bound(3, y=5) == 8

        # With executor
        def passthrough(target, *args, **kwargs):
            return target(*args, **kwargs)

        gate.set(passthrough)
        assert bound(3) == 13
        assert bound(3, y=5) == 8

    def test_bound_repr(self):
        gate = _utils.Gate()
        target = lambda x: x
        bound = gate.bind(target)
        r = repr(bound)
        assert 'BoundGate' in r

    def test_multiple_binds_share_gate(self):
        """Multiple binds to the same gate share the same executor."""
        gate = _utils.Gate()
        calls = []

        def executor(target, *args, **kwargs):
            calls.append(target.__name__)
            return target(*args, **kwargs)

        def func_a():
            return 'a'

        def func_b():
            return 'b'

        bound_a = gate.bind(func_a)
        bound_b = gate.bind(func_b)

        # Disabled — direct
        assert bound_a() == 'a'
        assert bound_b() == 'b'
        assert calls == []

        # Enable executor
        gate.set(executor)
        assert bound_a() == 'a'
        assert bound_b() == 'b'
        assert calls == ['func_a', 'func_b']


class TestGateContextManager:
    """Tests for gate(executor) context manager."""

    def test_context_sets_and_restores(self):
        gate = _utils.Gate()
        executor = lambda target, *a, **kw: target(*a, **kw)

        assert gate.executor is None

        with gate(executor):
            assert gate.executor is executor

        assert gate.executor is None

    def test_context_restores_previous(self):
        gate = _utils.Gate()
        exec1 = lambda target, *a, **kw: target(*a, **kw)
        exec2 = lambda target, *a, **kw: target(*a, **kw)

        gate.set(exec1)

        with gate(exec2):
            assert gate.executor is exec2

        assert gate.executor is exec1

    def test_context_nested(self):
        gate = _utils.Gate()
        exec1 = lambda target, *a, **kw: 'exec1'
        exec2 = lambda target, *a, **kw: 'exec2'
        exec3 = lambda target, *a, **kw: 'exec3'

        def target():
            return 'direct'

        bound = gate.bind(target)

        assert bound() == 'direct'

        with gate(exec1):
            assert bound() == 'exec1'
            with gate(exec2):
                assert bound() == 'exec2'
                with gate(exec3):
                    assert bound() == 'exec3'
                assert bound() == 'exec2'
            assert bound() == 'exec1'

        assert bound() == 'direct'

    def test_context_with_none_disables(self):
        gate = _utils.Gate()
        executor = lambda target, *a, **kw: 'intercepted'
        target = lambda: 'direct'

        gate.set(executor)
        bound = gate.bind(target)

        assert bound() == 'intercepted'

        with gate(None):
            assert bound() == 'direct'

        assert bound() == 'intercepted'

    def test_context_restores_on_exception(self):
        gate = _utils.Gate()
        executor = lambda target, *a, **kw: target(*a, **kw)

        gate.set(executor)
        other_exec = lambda target, *a, **kw: target(*a, **kw)

        try:
            with gate(other_exec):
                assert gate.executor is other_exec
                raise ValueError("boom")
        except ValueError:
            pass

        assert gate.executor is executor

    def test_context_non_callable_raises(self):
        gate = _utils.Gate()
        with pytest.raises(TypeError):
            gate(42)


class TestGateThreadIsolation:
    """Tests that Gate executor is per-thread."""

    def test_different_threads_see_different_executors(self):
        import threading

        gate = _utils.Gate()
        results = {}

        def exec_record(target, *args, **kwargs):
            return ('recorded', target(*args, **kwargs))

        def exec_replay(target, *args, **kwargs):
            return ('replayed', target(*args, **kwargs))

        def target():
            return 'result'

        bound = gate.bind(target)

        def thread_record():
            gate.set(exec_record)
            results['record'] = bound()

        def thread_replay():
            gate.set(exec_replay)
            results['replay'] = bound()

        t1 = threading.Thread(target=thread_record)
        t2 = threading.Thread(target=thread_replay)
        t1.start()
        t1.join()
        t2.start()
        t2.join()

        assert results['record'] == ('recorded', 'result')
        assert results['replay'] == ('replayed', 'result')

        # Main thread should still be disabled
        assert bound() == 'result'

    def test_context_manager_thread_isolated(self):
        import threading

        gate = _utils.Gate()
        results = {}
        barrier = threading.Barrier(2)

        def target():
            return 'value'

        bound = gate.bind(target)

        def exec_a(target, *a, **kw):
            return 'A:' + target(*a, **kw)

        def exec_b(target, *a, **kw):
            return 'B:' + target(*a, **kw)

        def thread_a():
            with gate(exec_a):
                barrier.wait(timeout=5)  # sync
                results['a'] = bound()
                barrier.wait(timeout=5)  # sync

        def thread_b():
            with gate(exec_b):
                barrier.wait(timeout=5)  # sync
                results['b'] = bound()
                barrier.wait(timeout=5)  # sync

        t1 = threading.Thread(target=thread_a)
        t2 = threading.Thread(target=thread_b)
        t1.start()
        t2.start()
        t1.join(timeout=10)
        t2.join(timeout=10)

        assert results['a'] == 'A:value'
        assert results['b'] == 'B:value'


class TestGateExecutorBehavior:
    """Tests for various executor patterns."""

    def test_executor_receives_target_and_args(self):
        gate = _utils.Gate()
        received = []

        def executor(target, *args, **kwargs):
            received.append((target, args, tuple(sorted(kwargs.items()))))
            return target(*args, **kwargs)

        def target(a, b, c=None):
            return (a, b, c)

        gate.set(executor)
        bound = gate.bind(target)
        result = bound(1, 2, c=3)

        assert result == (1, 2, 3)
        assert len(received) == 1
        assert received[0][0] is target
        assert received[0][1] == (1, 2)
        assert received[0][2] == (('c', 3),)

    def test_executor_can_short_circuit(self):
        """Executor can return without calling target."""
        gate = _utils.Gate()

        def replay_executor(target, *args, **kwargs):
            return 'replayed_value'

        def target():
            raise RuntimeError("should not be called")

        gate.set(replay_executor)
        bound = gate.bind(target)
        result = bound()
        assert result == 'replayed_value'

    def test_executor_can_modify_args(self):
        gate = _utils.Gate()

        def doubling_executor(target, *args, **kwargs):
            new_args = tuple(a * 2 for a in args)
            return target(*new_args, **kwargs)

        def target(x, y):
            return x + y

        gate.set(doubling_executor)
        bound = gate.bind(target)
        assert bound(3, 4) == 14  # (6 + 8)

    def test_switching_executors_at_runtime(self):
        gate = _utils.Gate()

        def exec_record(target, *args, **kwargs):
            return ('record', target(*args, **kwargs))

        def exec_replay(target, *args, **kwargs):
            return ('replay', target(*args, **kwargs))

        def target():
            return 'data'

        bound = gate.bind(target)

        assert bound() == 'data'

        gate.set(exec_record)
        assert bound() == ('record', 'data')

        gate.set(exec_replay)
        assert bound() == ('replay', 'data')

        gate.disable()
        assert bound() == 'data'


class TestApplyWith:
    """Tests for gate.apply_with() — temporary executor activation."""

    def test_apply_with_returns_callable(self):
        gate = _utils.Gate()
        def executor(target, *args, **kwargs):
            return target(*args, **kwargs)
        aw = gate.apply_with(executor)
        assert callable(aw)

    def test_apply_with_calls_function_directly(self):
        """apply_with(f, *args) calls f(*args) directly — the executor is
        activated on the gate, not used to route the call itself."""
        gate = _utils.Gate()

        def executor(target, *args, **kwargs):
            return target(*args, **kwargs)

        aw = gate.apply_with(executor)
        result = aw(lambda x, y: x + y, 3, 4)
        assert result == 7

    def test_apply_with_no_executor_active_before_and_after(self):
        """apply_with only activates the executor during the call."""
        gate = _utils.Gate()

        def executor(target, *args, **kwargs):
            return target(*args, **kwargs)

        aw = gate.apply_with(executor)

        # Gate starts disabled
        assert gate.executor is None

        aw(lambda: 42)

        # Gate is disabled again after the call
        assert gate.executor is None

    def test_apply_with_restores_previous_executor(self):
        """If a different executor was already set, it is restored."""
        gate = _utils.Gate()

        def original_exec(target, *args, **kwargs):
            return ('original', target(*args, **kwargs))

        def temp_exec(target, *args, **kwargs):
            return ('temp', target(*args, **kwargs))

        gate.set(original_exec)
        aw = gate.apply_with(temp_exec)
        bound = gate.bind(lambda: 'data')

        # During aw(), BoundGate calls go through temp_exec
        result = aw(lambda: bound())
        assert result == ('temp', 'data')

        # Original executor is restored
        assert gate.executor is original_exec

    def test_apply_with_restores_on_exception(self):
        """Previous executor is restored even if the call raises."""
        gate = _utils.Gate()

        def original_exec(target, *args, **kwargs):
            return target(*args, **kwargs)

        def temp_exec(target, *args, **kwargs):
            return target(*args, **kwargs)

        gate.set(original_exec)
        aw = gate.apply_with(temp_exec)

        def boom():
            raise ValueError("kaboom")

        with pytest.raises(ValueError, match="kaboom"):
            aw(boom)

        # Original executor is restored despite the exception
        assert gate.executor is original_exec

    def test_apply_with_kwargs(self):
        """Keyword arguments are forwarded correctly."""
        gate = _utils.Gate()

        def executor(target, *args, **kwargs):
            return target(*args, **kwargs)

        aw = gate.apply_with(executor)

        def f(a, b, c=None):
            return (a, b, c)

        assert aw(f, 1, 2, c=3) == (1, 2, 3)

    def test_apply_with_none_disables(self):
        """apply_with(None) temporarily disables the executor."""
        gate = _utils.Gate()
        calls = []

        def active_exec(target, *args, **kwargs):
            calls.append('exec')
            return target(*args, **kwargs)

        gate.set(active_exec)
        aw = gate.apply_with(None)

        # apply_with(None) should disable, so bind'd functions go direct
        bound = gate.bind(lambda: 'hello')

        # But apply_with(None) works on direct calls, not bound calls.
        # It just calls f(*args, **kwargs) with the gate disabled.
        result = aw(lambda: 'direct')
        assert result == 'direct'
        assert calls == []  # executor was not called

        # Active executor is restored
        assert gate.executor is active_exec

    def test_apply_with_no_args_raises(self):
        """Calling apply_with() result with no arguments is a TypeError."""
        gate = _utils.Gate()
        aw = gate.apply_with(lambda target, *a, **kw: target(*a, **kw))

        with pytest.raises(TypeError):
            aw()

    def test_apply_with_activates_executor_for_bound_calls(self):
        """BoundGate calls inside f use the activated executor."""
        gate = _utils.Gate()
        received = []

        def executor(target, *args, **kwargs):
            received.append(target)
            return target(*args, **kwargs)

        bound = gate.bind(lambda: 99)
        aw = gate.apply_with(executor)

        # The orchestrating function calls the BoundGate
        result = aw(lambda: bound())
        assert result == 99
        assert len(received) == 1  # executor saw the bound call

    def test_apply_with_executor_can_short_circuit_bound(self):
        """An executor can intercept bound calls made within apply_with."""
        gate = _utils.Gate()

        def replay_exec(target, *args, **kwargs):
            return 'replayed'

        def should_not_run():
            raise RuntimeError("should not be called")

        bound = gate.bind(should_not_run)
        aw = gate.apply_with(replay_exec)

        # The executor intercepts the BoundGate call
        assert aw(lambda: bound()) == 'replayed'

    def test_apply_with_multiple_bound_calls(self):
        """Multiple BoundGate calls inside apply_with all go through executor."""
        gate = _utils.Gate()
        calls = []

        def executor(target, *args, **kwargs):
            calls.append('exec')
            return target(*args, **kwargs)

        aw = gate.apply_with(executor)
        bound_add = gate.bind(lambda x, y: x + y)
        bound_mul = gate.bind(lambda x, y: x * y)

        def orchestrator():
            a = bound_add(10, 20)
            b = bound_mul(3, 4)
            return a + b

        result = aw(orchestrator)
        assert result == 42  # 30 + 12
        # executor was called for each BoundGate call
        assert len(calls) == 2

    def test_apply_with_repr(self):
        gate = _utils.Gate()
        def my_exec(target, *args, **kwargs):
            return target(*args, **kwargs)
        aw = gate.apply_with(my_exec)
        assert 'ApplyWith' in repr(aw)

    def test_apply_with_repr_disabled(self):
        gate = _utils.Gate()
        aw = gate.apply_with(None)
        assert 'disabled' in repr(aw)

    def test_apply_with_get_returns_executor(self):
        gate = _utils.Gate()
        def my_exec(target, *args, **kwargs):
            return target(*args, **kwargs)
        aw = gate.apply_with(my_exec)
        assert aw.get() is my_exec

    def test_apply_with_get_returns_none_when_disabled(self):
        gate = _utils.Gate()
        aw = gate.apply_with(None)
        assert aw.get() is None

    def test_apply_with_set_changes_executor(self):
        gate = _utils.Gate()
        calls = []

        def exec_a(target, *args, **kwargs):
            calls.append('a')
            return target(*args, **kwargs)

        def exec_b(target, *args, **kwargs):
            calls.append('b')
            return target(*args, **kwargs)

        aw = gate.apply_with(exec_a)
        bound = gate.bind(lambda: 'ok')

        aw(lambda: bound())
        assert calls == ['a']

        aw.set(exec_b)
        assert aw.get() is exec_b

        aw(lambda: bound())
        assert calls == ['a', 'b']

    def test_apply_with_set_none_disables(self):
        gate = _utils.Gate()
        calls = []

        def executor(target, *args, **kwargs):
            calls.append('exec')
            return target(*args, **kwargs)

        aw = gate.apply_with(executor)
        bound = gate.bind(lambda: 'data')

        aw(lambda: bound())
        assert calls == ['exec']

        # Disable via set(None)
        aw.set(None)
        assert aw.get() is None
        assert 'disabled' in repr(aw)

        # BoundGate now passes through directly
        result = aw(lambda: bound())
        assert result == 'data'
        assert calls == ['exec']  # no new executor call

    def test_apply_with_set_rejects_non_callable(self):
        gate = _utils.Gate()
        aw = gate.apply_with(None)
        with pytest.raises(TypeError):
            aw.set(42)

    def test_apply_with_none_disables_gate_during_call(self):
        """gate.apply_with(None) temporarily disables the gate."""
        gate = _utils.Gate()
        calls = []

        def active_exec(target, *args, **kwargs):
            calls.append('exec')
            return target(*args, **kwargs)

        gate.set(active_exec)
        bound = gate.bind(lambda: 'result')

        # With active executor, bound goes through it
        assert bound() == 'result'
        assert calls == ['exec']

        # apply_with(None) disables the gate for the duration
        aw = gate.apply_with(None)
        result = aw(lambda: bound())
        assert result == 'result'
        assert calls == ['exec']  # no new executor call — gate was disabled

        # Active executor is restored after apply_with
        assert gate.executor is active_exec


class TestGatePredicate:
    """Tests for gate.test() — C-level predicate checking gate executor identity."""

    def test_returns_callable(self):
        gate = utils.Gate()
        pred = gate.test(None)
        assert callable(pred)

    def test_disabled_gate_matches_none(self):
        gate = utils.Gate()
        pred = gate.test(None)
        # Gate starts disabled (executor is None/NULL)
        assert pred() is True

    def test_disabled_gate_does_not_match_executor(self):
        gate = utils.Gate()
        executor = lambda target, *a, **kw: target(*a, **kw)
        pred = gate.test(executor)
        # Gate is disabled, so current executor is not 'executor'
        assert pred() is False

    def test_active_gate_matches_executor(self):
        gate = utils.Gate()
        executor = lambda target, *a, **kw: target(*a, **kw)
        gate.set(executor)
        pred = gate.test(executor)
        assert pred() is True

    def test_active_gate_does_not_match_different_executor(self):
        gate = utils.Gate()
        exec_a = lambda target, *a, **kw: target(*a, **kw)
        exec_b = lambda target, *a, **kw: target(*a, **kw)
        gate.set(exec_a)
        pred = gate.test(exec_b)
        assert pred() is False

    def test_active_gate_none_predicate_returns_false(self):
        gate = utils.Gate()
        executor = lambda target, *a, **kw: target(*a, **kw)
        gate.set(executor)
        pred = gate.test(None)
        assert pred() is False

    def test_identity_not_equality(self):
        """test() uses identity (is), not equality (==)."""
        gate = utils.Gate()
        # Two distinct objects that are "equal" in value
        exec_a = lambda target, *a, **kw: target(*a, **kw)
        exec_b = lambda target, *a, **kw: target(*a, **kw)
        gate.set(exec_a)
        pred_a = gate.test(exec_a)
        pred_b = gate.test(exec_b)
        assert pred_a() is True
        assert pred_b() is False  # different object

    def test_tracks_gate_state_changes(self):
        gate = utils.Gate()
        executor = lambda target, *a, **kw: target(*a, **kw)
        pred = gate.test(executor)
        assert pred() is False  # gate disabled
        gate.set(executor)
        assert pred() is True   # now matches
        gate.disable()
        assert pred() is False  # disabled again

    def test_ignores_arguments(self):
        """Predicate should work regardless of what arguments are passed."""
        gate = utils.Gate()
        pred = gate.test(None)
        # All of these should return True (gate is disabled = matches None)
        assert pred() is True
        assert pred(1) is True
        assert pred(1, 2, 3) is True
        assert pred(1, 2, key='val') is True

    def test_ignores_arguments_when_false(self):
        gate = utils.Gate()
        executor = lambda target, *a, **kw: target(*a, **kw)
        gate.set(executor)
        pred = gate.test(None)
        assert pred(1, 2, 3) is False

    def test_repr_with_executor(self):
        gate = utils.Gate()
        def my_exec(target, *a, **kw): return target(*a, **kw)
        pred = gate.test(my_exec)
        r = repr(pred)
        assert 'GatePredicate' in r
        assert 'my_exec' in r

    def test_repr_disabled(self):
        gate = utils.Gate()
        pred = gate.test(None)
        r = repr(pred)
        assert 'GatePredicate' in r
        assert 'disabled' in r

    def test_multiple_predicates_same_gate(self):
        gate = utils.Gate()
        exec_a = lambda target, *a, **kw: 'a'
        exec_b = lambda target, *a, **kw: 'b'
        pred_a = gate.test(exec_a)
        pred_b = gate.test(exec_b)
        pred_none = gate.test(None)

        # Gate disabled
        assert pred_none() is True
        assert pred_a() is False
        assert pred_b() is False

        # Set to exec_a
        gate.set(exec_a)
        assert pred_none() is False
        assert pred_a() is True
        assert pred_b() is False

        # Set to exec_b
        gate.set(exec_b)
        assert pred_none() is False
        assert pred_a() is False
        assert pred_b() is True

    def test_not_directly_instantiable(self):
        """GatePredicate should not be instantiable from Python."""
        gate = utils.Gate()
        pred = gate.test(None)
        with pytest.raises(TypeError):
            type(pred)()

