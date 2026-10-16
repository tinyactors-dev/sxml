<?xml version="1.0" encoding="ISO-8859-1"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="3.0">
  
  <!--Test node ordering with namespace nodes. Saxon bug 7198 -->

  <xsl:template name="xsl:initial-template">
    <xsl:variable name="doc"><doc><x>abc</x></doc></xsl:variable>
    <xsl:variable name="x" select="$doc//x"/>
    <out><xsl:value-of select="($x | $x/namespace::* | $x/text()) ! (name() || '=' || string())"/></out>
  </xsl:template>
  
</xsl:stylesheet>
