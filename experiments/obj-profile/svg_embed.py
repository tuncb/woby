"""Keep inline SVG definition IDs unique across report figures."""
import re


def namespace_svg(svg, prefix):
    identifiers = set(re.findall(r'\bid="([^"]+)"', svg))
    def reference(match):
        before, identifier, after = match.groups()
        return before + (prefix + identifier if identifier in identifiers else identifier) + after
    svg = re.sub(r'(\bid=")([^"]+)(")', reference, svg)
    svg = re.sub(r'((?:xlink:)?href="#)([^" ]+)(")', reference, svg)
    return re.sub(r'(url\(#)([^)]+)(\))', reference, svg)
