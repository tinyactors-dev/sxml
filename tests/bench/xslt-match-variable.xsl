<?xml version="1.0"?>
<!-- Template rules whose pattern is a variable reference, match="$var"
     (XSLT 4.0, 5.5.2, D185): every element of the document is matched
     against the set of rich persons. -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:variable name="rich" select="//person[number(profile/@income) gt 50000]"/>
  <xsl:template match="/">
    <persons>
      <xsl:apply-templates select="//*"/>
    </persons>
  </xsl:template>
  <xsl:template match="$rich">
    <rich id="{@id}"/>
  </xsl:template>
  <xsl:template match="*"/>
</xsl:stylesheet>
