<?xml version="1.0"?>
<!-- sxml transform: xsl:sort with lang over the CLDR collations the
     command embeds (D220): Spanish sorts n-tilde as a letter after n,
     the root order as n with a mark; caseFirst through case-order. -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>
  <xsl:variable name="words" select="('oso', '&#xF1;u', 'nube', 'Nube', 'nz')"/>
  <xsl:template match="/">
    <xsl:for-each select="$words">
      <xsl:sort select="." lang="es"/>
      <xsl:value-of select="."/>
      <xsl:text> </xsl:text>
    </xsl:for-each>
    <xsl:text>&#10;</xsl:text>
    <xsl:for-each select="$words">
      <xsl:sort select="." lang="en"/>
      <xsl:value-of select="."/>
      <xsl:text> </xsl:text>
    </xsl:for-each>
    <xsl:text>&#10;</xsl:text>
    <xsl:for-each select="$words">
      <xsl:sort select="." lang="es" case-order="upper-first"/>
      <xsl:value-of select="."/>
      <xsl:text> </xsl:text>
    </xsl:for-each>
    <xsl:text>&#10;</xsl:text>
  </xsl:template>
</xsl:stylesheet>
