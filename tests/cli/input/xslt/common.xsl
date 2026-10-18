<?xml version="1.0"?>
<!-- Included by books.xsl. -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:x="urn:x">
  <xsl:template name="label">
    <xsl:value-of select="title, '(' || x:price || ')'"/>
  </xsl:template>
</xsl:stylesheet>
