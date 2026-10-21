<?xml version="1.0" encoding="UTF-8"?>
<!-- Scales an SVG drawing: the width and height of the root element are
     multiplied by the parameter "factor"; the viewBox stays, so the
     drawing grows or shrinks with them. A drawing without a viewBox gets
     one first, from its width and height, or it would be cut off instead. -->
<xsl:stylesheet version="4.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:svg="http://www.w3.org/2000/svg">

  <xsl:param name="factor" as="xs:decimal" select="2"/>

  <xsl:mode on-no-match="shallow-copy"/>
  <xsl:output omit-xml-declaration="yes"/>

  <xsl:template match="/svg:svg[not(@viewBox)]">
    <xsl:copy>
      <xsl:attribute name="viewBox" select="0, 0, @width, @height"/>
      <xsl:apply-templates select="@* | node()"/>
    </xsl:copy>
  </xsl:template>

  <xsl:template match="/svg:svg/@width | /svg:svg/@height">
    <xsl:attribute name="{name()}" select="xs:decimal(.) * $factor"/>
  </xsl:template>

</xsl:stylesheet>
