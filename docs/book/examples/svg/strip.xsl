<?xml version="1.0" encoding="UTF-8"?>
<!-- Removes what an SVG editor leaves in a drawing: comments, metadata,
     elements and attributes in the editor's namespaces, and every id that
     nothing refers to. A reference is "#id" in an attribute (url(#id),
     href="#id") or in the style text. -->
<xsl:stylesheet version="4.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:svg="http://www.w3.org/2000/svg"
    exclude-result-prefixes="#all">

  <xsl:mode on-no-match="shallow-copy"/>
  <xsl:output omit-xml-declaration="yes" indent="yes"/>
  <xsl:strip-space elements="*"/>
  <xsl:preserve-space elements="svg:text svg:style"/>

  <!-- The ids that something refers to. -->
  <xsl:variable name="referenced" select="
    distinct-values(
      tokenize(string-join((//@*, //svg:style), ' '), '[^\w#.-]+')
        [starts-with(., '#')] ! substring(., 2)
    )"/>

  <!-- Elements in the SVG namespace are copied without the namespace
       declarations they don't use. -->
  <xsl:template match="svg:*">
    <xsl:copy copy-namespaces="no">
      <xsl:apply-templates select="@* | node()"/>
    </xsl:copy>
  </xsl:template>

  <!-- Dropped: metadata, comments, elements in other namespaces,
       attributes in a namespace, and ids nothing refers to. A union
       pattern has the lowest priority of its parts (comment(), -0.5), so
       the rule says its priority, to win over svg:* (-0.25). -->
  <xsl:template match="svg:metadata | comment() | *[namespace-uri() ne 'http://www.w3.org/2000/svg']"
                priority="1"/>
  <xsl:template match="@*[namespace-uri()] | @id[not(. = $referenced)]"/>

</xsl:stylesheet>
