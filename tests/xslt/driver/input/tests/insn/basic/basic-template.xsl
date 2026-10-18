<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
   <!-- The initial template with a parameter and a tunnel parameter. -->
   <xsl:template name="main">
      <xsl:param name="p" select="0"/>
      <xsl:param name="t" select="0" tunnel="yes"/>
      <out><xsl:value-of select="if ($p = 1 and $t = 2) then name(/doc/*) else 'wrong'"/></out>
   </xsl:template>
</xsl:stylesheet>
