<?xml version="1.0" encoding="UTF-8"?>
<!-- Lists the boxes of an SVG drawing: where each one is, and its text. -->
<xsl:stylesheet version="4.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xpath-default-namespace="http://www.w3.org/2000/svg"
    expand-text="yes">

  <xsl:output method="text"/>

  <xsl:template match="/">
    <xsl:apply-templates select="//rect"/>
  </xsl:template>

  <xsl:template match="rect">
    <xsl:text>{@x},{@y} {../text}&#10;</xsl:text>
  </xsl:template>

</xsl:stylesheet>
