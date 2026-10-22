"""Parse the Provider contract: exactly one GVariant string containing JSON.

No general GVariant evaluator or undocumented gdbus JSON option is required.
Other wire types are rejected as a Provider error, not a filesystem scan error.
"""
import ast
import json


def parse(payload):
    value=payload.decode('utf-8').strip()
    for _ in range(8):
        if value.startswith('(') and value.endswith(')'):
            value=value[1:-1].strip()
            if value.endswith(','):value=value[:-1].rstrip()
        elif value.startswith('<') and value.endswith('>'):
            value=value[1:-1].strip()
        elif value.startswith('@s '):value=value[3:].lstrip()
        elif value.startswith('string '):value=value[7:].lstrip()
        else:break
    if not value or value[0] not in ('"', "'"):
        raise ValueError('D-Bus Provider must return one JSON string')
    try:decoded=ast.literal_eval(value)
    except (SyntaxError,ValueError) as error:raise ValueError('Invalid D-Bus Provider string') from error
    if not isinstance(decoded,str):raise ValueError('D-Bus Provider must return one JSON string')
    return json.loads(decoded)
