from __future__ import annotations

import ast
import math
from pathlib import Path
from typing import Any, Dict

_ALLOWED_BIN_OPS = {
    ast.Add: lambda a, b: a + b,
    ast.Sub: lambda a, b: a - b,
    ast.Mult: lambda a, b: a * b,
    ast.Div: lambda a, b: a / b,
    ast.FloorDiv: lambda a, b: a // b,
    ast.Mod: lambda a, b: a % b,
    ast.Pow: lambda a, b: a**b,
    ast.BitXor: lambda a, b: a**b,  # interpret caret as exponent
}

_ALLOWED_UNARY_OPS = {
    ast.UAdd: lambda a: +a,
    ast.USub: lambda a: -a,
}

_NAME_CONSTANTS: Dict[str, Any] = {
    "pi": math.pi,
    "e": math.e,
    "tau": math.tau,
    "true": True,
    "false": False,
}


class ExpressionError(ValueError):
    """Raised when an expression cannot be evaluated safely."""


def evaluate_expression(expr: str) -> Any:
    """
    Evaluate a mathematical expression using a restricted AST.

    Supports numeric literals, booleans, and the operators +, -, *, /, //, %, **, ^.
    """

    try:
        tree = ast.parse(expr, mode="eval")
    except SyntaxError as exc:
        raise ExpressionError(str(exc)) from exc

    value = _eval_node(tree.body)
    return _coerce_numeric(value)


def _eval_node(node: ast.AST) -> Any:
    if isinstance(node, ast.Constant):
        return node.value
    if isinstance(node, ast.Num):  # pragma: no cover (py<3.8 compatibility)
        return node.n
    if isinstance(node, ast.BinOp):
        op_type = type(node.op)
        if op_type not in _ALLOWED_BIN_OPS:
            raise ExpressionError(f"Operator {op_type.__name__} not allowed")
        return _ALLOWED_BIN_OPS[op_type](_eval_node(node.left), _eval_node(node.right))
    if isinstance(node, ast.UnaryOp):
        op_type = type(node.op)
        if op_type not in _ALLOWED_UNARY_OPS:
            raise ExpressionError(f"Unary operator {op_type.__name__} not allowed")
        return _ALLOWED_UNARY_OPS[op_type](_eval_node(node.operand))
    if isinstance(node, ast.Name):
        name = node.id.lower()
        if name not in _NAME_CONSTANTS:
            raise ExpressionError(f"Unknown constant '{node.id}'")
        return _NAME_CONSTANTS[name]
    if isinstance(node, ast.Expression):
        return _eval_node(node.body)
    raise ExpressionError(f"Unsupported expression node: {type(node).__name__}")


def _coerce_numeric(value: Any) -> Any:
    if isinstance(value, float):
        if math.isfinite(value) and value.is_integer():
            return int(value)
        return value
    return value


def ensure_parent_dir(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
