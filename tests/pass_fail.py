#!/usr/bin/env python3

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable


class TestFailure(Exception):
    pass


def fail(message: str) -> None:
    raise TestFailure(message)


def expect(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def expect_equal(actual, expected, label: str) -> None:
    if actual != expected:
        fail(f"{label}: expected {expected!r}, got {actual!r}")


def expect_contains(text: str, needle: str, label: str) -> None:
    if needle not in text:
        fail(f"{label}: missing {needle!r}")


@dataclass
class TestCase:
    name: str
    run: Callable[[], None]


class TestSuite:
    def __init__(self, name: str):
        self.name = name
        self.cases: list[TestCase] = []

    def add(self, name: str, run: Callable[[], None]) -> None:
        self.cases.append(TestCase(name=name, run=run))

    def execute(self) -> int:
        passed = 0
        failed = 0
        print(f"== {self.name} ==")
        for case in self.cases:
            try:
                case.run()
                passed += 1
                print(f"PASS {case.name}")
            except TestFailure as ex:
                failed += 1
                print(f"FAIL {case.name}")
                print(f"  {ex}")
            except Exception as ex:  # pragma: no cover - test harness fallback
                failed += 1
                print(f"FAIL {case.name}")
                print(f"  unexpected error: {ex}")
        print(f"summary: {passed} passed, {failed} failed")
        return 0 if failed == 0 else 1
