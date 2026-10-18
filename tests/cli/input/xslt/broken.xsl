<?xml version="1.0"?>
<!-- A static error: xsl:value-of has a select that does not parse. -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:template match="/">
    <xsl:value-of select="1 +"/>
  </xsl:template>
</xsl:stylesheet>
