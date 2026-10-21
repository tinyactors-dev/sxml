<?xml version="1.0" encoding="UTF-8"?>
<!-- Highlights one node of a drawing made by draw.xqm, and the edges that
     start or end at it; the rest of the drawing fades. The node is the
     parameter "node", a node id of the description. -->
<xsl:stylesheet version="4.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xpath-default-namespace="http://www.w3.org/2000/svg"
    expand-text="yes">

  <xsl:param name="node" as="xs:string" required="yes"/>

  <xsl:mode on-no-match="shallow-copy"/>
  <xsl:output omit-xml-declaration="yes"/>

  <!-- The node ids of the drawing, from the groups' ids "node-ID". -->
  <xsl:variable name="ids" select="//g[starts-with(@id, 'node-')] ! substring(@id, 6)"/>

  <!-- The ids of the groups to highlight: the node and its edges, whose
       ids are "edge-FROM-TO". -->
  <xsl:variable name="highlighted" select="
    'node-' || $node,
    $ids ! ('edge-' || $node || '-' || ., 'edge-' || . || '-' || $node)"/>

  <xsl:template match="/">
    <xsl:if test="not($node = $ids)">
      <xsl:message>highlight.xsl: the drawing has no node "{$node}"</xsl:message>
    </xsl:if>
    <xsl:apply-templates/>
  </xsl:template>

  <xsl:template match="style/text()">
    <xsl:value-of select="."/>
    <xsl:text>  .node:not(.highlight), .edge:not(.highlight) {{ opacity: 0.3 }}
    .highlight rect, .highlight line {{ stroke: #d81b60; stroke-width: 3 }}
  </xsl:text>
  </xsl:template>

  <xsl:template match="g[@id = $highlighted]">
    <xsl:copy>
      <xsl:apply-templates select="@*"/>
      <xsl:attribute name="class" select="@class, 'highlight'"/>
      <xsl:apply-templates/>
    </xsl:copy>
  </xsl:template>

</xsl:stylesheet>
