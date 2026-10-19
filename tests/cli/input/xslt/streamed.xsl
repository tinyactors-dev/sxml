<?xml version="1.0"?>
<!-- A streamable mode (XSLT 4.0, 6.9.1): sxml transform streams INPUT
     (D228). The template rule for "/" reads the books through one stream
     point; the rule for book reads its node through a streamed path. -->
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:x="urn:x"
                version="3.0">
  <xsl:mode streamable="yes" on-no-match="shallow-skip"/>
  <xsl:output indent="no"/>
  <xsl:template match="/">
    <titles>
      <xsl:apply-templates select="catalog/book"/>
    </titles>
  </xsl:template>
  <xsl:template match="book">
    <title id="{@id}" position="{position()}"><xsl:value-of select="title"/></title>
  </xsl:template>
</xsl:stylesheet>
