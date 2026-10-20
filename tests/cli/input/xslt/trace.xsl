<?xml version="1.0"?>
<!-- fn:trace and fn:message write to standard error; xsl:message too. -->
<xsl:stylesheet version="4.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:template name="xsl:initial-template">
    <xsl:message>starting</xsl:message>
    <xsl:value-of select="trace(count(doc('../books.xml')//book), 'books'), message('done')"/>
  </xsl:template>
</xsl:stylesheet>
