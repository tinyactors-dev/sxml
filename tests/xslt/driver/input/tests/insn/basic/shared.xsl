<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
   <!-- Applied to the initial match selection: one number per item. -->
   <xsl:param name="global" static="yes"/>
   <xsl:template match="a">
      <xsl:sequence select="1"/>
   </xsl:template>
</xsl:stylesheet>
