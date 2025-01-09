import re
import os
import argparse as ap


ast_pattern = re.compile("class (Ast[_a-zA-Z0-9]+)")


# void AstProgram::accept(AstVisitor & visitor) { visitor.visit(this); }
def generate_ast_accept(class_lst):
    output =""
    output += """\

#include "ast/astnode.hh"

namespace lona
{

#define DEF_ACCEPT(classname) void classname::accept(AstVisitor & visitor) { visitor.visit(this); }

"""
    for class_name in class_lst:
        output += f"DEF_ACCEPT({class_name})\n"
    output += """\
}
"""
    return output

def get_class_ast(filename):
    lst = []
    with open(filename, 'r') as f:
        for line in f.readlines():
            match = ast_pattern.match(line)
            if match:
                lst.append(match.group(1))
    return lst

parser = ap.ArgumentParser(description="Generate Ast accept methods")
parser.add_argument("filename", help="path to astnode.hh")
args = parser.parse_args()

if os.path.isfile(args.filename):
    class_lst = get_class_ast(args.filename)
    print(generate_ast_accept(class_lst))
else:
    print("File not found")












