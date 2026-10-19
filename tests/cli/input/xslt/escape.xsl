<?xml version="1.0"?>
<!-- A result document at an href a parameter gives; sxml refuses one
     outside the result directory (sxml transform with a result directory). -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:param name="href" select="'../outside.xml'"/>
  <xsl:template name="xsl:initial-template">
    <xsl:result-document href="{$href}"><a/></xsl:result-document>
  </xsl:template>
</xsl:stylesheet>
