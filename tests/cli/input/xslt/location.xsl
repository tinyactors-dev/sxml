<?xml version="1.0"?>
<!-- fn:location of the stylesheet's input, whose whitespace is stripped
     (D313, D318): each book's line and column. -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:strip-space elements="*"/>
  <xsl:output method="text"/>
  <xsl:template match="/">
    <xsl:for-each select="//book">
      <xsl:value-of select="@id, location(.)?line-number, location(.)?column-number"/>
      <xsl:text>&#10;</xsl:text>
    </xsl:for-each>
  </xsl:template>
</xsl:stylesheet>
