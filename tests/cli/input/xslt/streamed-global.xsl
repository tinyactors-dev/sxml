<?xml version="1.0"?>
<!-- A streamed principal source is no global context item (XSLT 4.0,
     2.4.2; D228): a global variable that reads the focus fails. -->
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="3.0">
  <xsl:mode streamable="yes"/>
  <xsl:variable name="root" select="name(/*)"/>
  <xsl:template match="/">
    <xsl:value-of select="$root"/>
  </xsl:template>
</xsl:stylesheet>
