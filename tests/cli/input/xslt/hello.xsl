<?xml version="1.0"?>
<!-- No INPUT: xsl:initial-template, or the template named on the command line, is called;
     messages go to standard error. -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:param name="who" select="'world'"/>
  <xsl:output method="text"/>
  <xsl:template name="xsl:initial-template">
    <xsl:message>starting</xsl:message>
    <xsl:value-of select="'Hello, ' || $who || '!'"/>
  </xsl:template>
  <xsl:template name="numbers">
    <xsl:for-each-group select="1 to 10" group-adjacent=". idiv 4">
      <xsl:value-of select="current-group()" separator="+"/>
      <xsl:text>&#10;</xsl:text>
    </xsl:for-each-group>
  </xsl:template>
</xsl:stylesheet>
