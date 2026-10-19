<?xml version="1.0"?>
<!-- The built-in shallow-copy rule of a streamable mode copies the
     document as it streams (6.10.5, D228); a template rule that reads
     more of its node than a streamed path gets its node's subtree. -->
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" xmlns:x="urn:x"
                version="3.0">
  <xsl:mode streamable="yes" on-no-match="shallow-copy"/>
  <xsl:template match="x:price">
    <x:price currency="EUR"><xsl:value-of select=". * 2"/></x:price>
  </xsl:template>
  <xsl:template match="title">
    <xsl:copy-of select="."/>
  </xsl:template>
</xsl:stylesheet>
