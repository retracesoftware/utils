import sys
from pathlib import Path

import pytest

_utils = pytest.importorskip("retracesoftware_utils")

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

